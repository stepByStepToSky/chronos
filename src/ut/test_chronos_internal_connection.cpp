/**
 * @file test_chronos_internal_connection.cpp
 *
 * Copyright (C) Metaswitch Networks 2016
 * If license terms are provided to you in a COPYING file in the root directory
 * of the source code repository by which you are accessing this code, then
 * the license outlined in that COPYING file applies to your use.
 * Otherwise no rights are granted except for those provided to you by
 * Metaswitch Networks in a separate written agreement.
 */

#include "gtest/gtest.h"

#include "fakehttpresolver.hpp"
#include "chronos_internal_connection.h"
#include "base.h"
#include "mock_replicator.h"
#include "mock_timer_handler.h"
#include "globals.h"
#include "fakesnmp.hpp"
#include "test_interposer.hpp"
#include "mock_http_request.h"
#include "constants.h"


using namespace std;
using ::testing::_;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::Mock;

static SNMP::U32Scalar _fake_scalar("","");
static SNMP::CounterTable* _fake_counter_table;
static HttpResponse resp_accepted(HTTP_ACCEPTED, "", {});

MATCHER(IsTombstone, "is a tombstone")
{
  return arg->is_tombstone();
}

MATCHER(IsNotTombstone, "is not a tombstone")
{
  return !(arg->is_tombstone());
}

// We need to subclass ChronosInernalConnection so that we can test it using
// MockHttpRequests.
// To do this, we make it a Mock itself, which will allow us to:
//   * check that build_request_proxy is called with the correct arguments
//   * return a pointer to a MockHttpRequest from build_request_proxy, which we
//     can then check has the correct methods called on it
class TestChronosInternalConnection : public ChronosInternalConnection
{
  TestChronosInternalConnection(HttpResolver* resolver,
                                TimerHandler* handler,
                                Replicator* replicator,
                                Alarm* alarm,
                                SNMP::U32Scalar* _remaining_nodes_scalar,
                                SNMP::CounterTable* _timers_processed_table,
                                SNMP::CounterTable* _invalid_timers_processed_table)
    : ChronosInternalConnection(resolver,
                                handler,
                                replicator,
                                alarm,
                                _remaining_nodes_scalar,
                                _timers_processed_table,
                                _invalid_timers_processed_table,
                                false)
  {
  }

  virtual ~TestChronosInternalConnection() {};

  virtual std::unique_ptr<HttpRequest> build_request(
                            const std::string& server,
                            const std::string& path,
                            HttpClient::RequestType method) override
  {
    std::unique_ptr<HttpRequest> req(build_request_proxy(server, path, method));
    return req;
  }

  MOCK_METHOD3(build_request_proxy, HttpRequest*(const std::string& server,
                                                 const std::string& path,
                                                 HttpClient::RequestType method));
};

/// Fixture for ChronosInternalConnectionTest.
class ChronosInternalConnectionTest : public Base
{
protected:
  void SetUp()
  {
    Base::SetUp();

    cwtest_completely_control_time();

    _fake_counter_table = SNMP::CounterTable::create("","");
    _resolver = new FakeHttpResolver("10.42.42.42");
    _replicator = new MockReplicator();
    _th = new MockTimerHandler();

    _chronos = new TestChronosInternalConnection(_resolver,
                                                 _th,
                                                 _replicator,
                                                 NULL,
                                                 &_fake_scalar,
                                                 _fake_counter_table,
                                                 _fake_counter_table);
    __globals->get_cluster_staying_addresses(_cluster_addresses);
    __globals->get_cluster_local_ip(_local_ip);

    Mock::VerifyAndClear(_chronos);
  }

  void TearDown()
  {
    delete _chronos;
    delete _th;
    delete _replicator;
    delete _resolver;
    delete _fake_counter_table;

    cwtest_reset_time();

    Base::TearDown();
  }

  // Template function for expecting a DELETE request to the specified server
  // for the specified timer
  void expect_delete(const std::string& server, const std::string& timer)
  {
    MockHttpRequest* mock_req = new MockHttpRequest();
    EXPECT_CALL(*_chronos, build_request_proxy(server,
                                               "/timers/references",
                                               HttpClient::RequestType::DELETE))
      .WillOnce(Return(mock_req)).RetiresOnSaturation();
    EXPECT_CALL(*mock_req, set_body(timer)).Times(1);
    EXPECT_CALL(*mock_req, send()).WillOnce(Return(resp_accepted));
  }

  FakeHttpResolver* _resolver;
  MockReplicator* _replicator;
  MockTimerHandler* _th;
  TestChronosInternalConnection* _chronos;
  std::vector<std::string> _cluster_addresses;
  std::string _local_ip;
};

TEST_F(ChronosInternalConnectionTest, SendDelete)
{
  MockHttpRequest* mock_req = new MockHttpRequest();
  HttpResponse ok(HTTP_OK, "{}", {});

  EXPECT_CALL(*_chronos, build_request_proxy("10.42.42.42:80",
                                             "/timers/references",
                                             HttpClient::RequestType::DELETE))
    .WillOnce(Return(mock_req));

  EXPECT_CALL(*mock_req, set_body("{}")).Times(1);
  EXPECT_CALL(*mock_req, send()).WillOnce(Return(ok));

  HTTPCode status = _chronos->send_delete("10.42.42.42:80", "{}");

  // Check that we get the correct response
  EXPECT_EQ(200, status);
}

TEST_F(ChronosInternalConnectionTest, SendGet)
{
  std::string response;
  bool use_time_from = true;

  MockHttpRequest* mock_req = new MockHttpRequest();
  HttpResponse resp(HTTP_OK, "response-body", {});

  // Expect that we'll build a GET request with the correct path and server
  EXPECT_CALL(*_chronos, build_request_proxy("10.42.42.42:80",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id;time-from=10000",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(mock_req));

  std::string range_header = std::string(HEADER_RANGE) + ":" +
                             std::to_string(MAX_TIMERS_IN_RESPONSE);

  // Expect that we add the correct range header to the request, then send it
  EXPECT_CALL(*mock_req, add_header(range_header)).Times(1);
  EXPECT_CALL(*mock_req, send()).WillOnce(Return(resp));

  std::string path = _chronos->create_path("10.0.0.1:9999", "cluster-view-id", 10000, use_time_from);
  HTTPCode status = _chronos->send_get("10.42.42.42:80", path, 0, response);

  // Check that we got the correct response
  EXPECT_EQ(200, status);
  EXPECT_EQ("response-body", response);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerNoResults)
{
  MockHttpRequest* resync_mock_req = new MockHttpRequest();
  HttpResponse resp(HTTP_OK, "{\"Timers\":[]}", {});

  // Expect that we'll build the GET request for the resync
  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req));

  // Expect that we'll add a header and send this request
  EXPECT_CALL(*resync_mock_req, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req, send()).WillOnce(Return(resp));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);

  // Check we got the correct response
  EXPECT_EQ(200, status);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerOneTimer)
{
  Timer* added_timer;

  MockHttpRequest* resync_mock_req = new MockHttpRequest();
  HttpResponse resp(HTTP_OK, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\", \"10.0.0.3:9999\"], \"Timer\": {\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [ \"10.0.0.1:9999\", \"10.0.0.3:9999\" ] }}}]}", {});
  // Expect that we'll build the GET request for the resync, and then three DELETE
  // requests which we send to each of the cluster nodes
  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req));

  // Expect that we'll add a header and send the GET request
  EXPECT_CALL(*resync_mock_req, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req, send()).WillOnce(Return(resp));

  // Expect that the delete requests are sent with the correct body
  expect_delete("10.0.0.1:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.2:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.3:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");

  // Expect that we'll add the timer, and save it off
  EXPECT_CALL(*_th, add_timer(_,_)).WillOnce(SaveArg<0>(&added_timer));

  // Expect that the timer is replicated
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsNotTombstone(), "10.0.0.3:9999")); // Update
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsTombstone(), "10.0.0.2:9999")); // Tombstone

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(200, status);

  delete added_timer; added_timer = NULL;
}

TEST_F(ChronosInternalConnectionTest, SendTriggerOneTimerDeleteError)
{
  Timer* added_timer;

  MockHttpRequest* resync_mock_req = new MockHttpRequest();
  HttpResponse resp(HTTP_OK, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\", \"10.0.0.3:9999\"], \"Timer\": {\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [ \"10.0.0.1:9999\", \"10.0.0.3:9999\" ] }}}]}", {});
  // Expect that we'll build the GET request for the resync, and then three DELETE
  // requests which we send to each of the cluster nodes
  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req));

  // Expect that we'll add a header and send the GET request
  EXPECT_CALL(*resync_mock_req, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req, send()).WillOnce(Return(resp));

  // Expect that the delete requests are sent with the correct body, and one of
  // them returns a 503
  expect_delete("10.0.0.1:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.2:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");

  MockHttpRequest* mock_delete = new MockHttpRequest();
  HttpResponse resp_error(HTTP_SERVER_UNAVAILABLE, "{}", {});

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.3:9999",
                                             "/timers/references",
                                             HttpClient::RequestType::DELETE))
    .WillOnce(Return(mock_delete));
  EXPECT_CALL(*mock_delete, set_body("{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}")).Times(1);
  EXPECT_CALL(*mock_delete, send()).WillOnce(Return(resp_error));

  // Expect that we'll add the timer, and save it off
  EXPECT_CALL(*_th, add_timer(_,_)).WillOnce(SaveArg<0>(&added_timer));

  // Expect that the timer is replicated
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsNotTombstone(), "10.0.0.3:9999")); // Update
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsTombstone(), "10.0.0.2:9999")); // Tombstone

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(200, status);

  delete added_timer; added_timer = NULL;
}

TEST_F(ChronosInternalConnectionTest, SendTriggerOneTimerWithTombstoneAndLeaving)
{
  // Set leaving addresses in globals so that we look there as well.
  std::vector<std::string> leaving_cluster_addresses;
  leaving_cluster_addresses.push_back("10.0.0.4:9999");

  __globals->set_cluster_leaving_addresses(leaving_cluster_addresses);
  _cluster_addresses.push_back("10.0.0.4:9999");

  MockHttpRequest* resync_mock_req = new MockHttpRequest();
  HttpResponse resp(HTTP_OK, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\", \"10.0.0.4:9999\"], \"Timer\": {\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [ \"10.0.0.1:9999\", \"10.0.0.3:9999\" ] }}}]}", {});

  Timer* added_timer;

  // Expect that we'll build the GET request for the resync, and then four DELETE
  // requests which we send to each of the cluster nodes
  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req));

  // Expect that we'll add a header and send the GET request
  EXPECT_CALL(*resync_mock_req, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req, send()).WillOnce(Return(resp));

  // Expect that we'll add the timer, and save it off
  EXPECT_CALL(*_th, add_timer(_,_)).WillOnce(SaveArg<0>(&added_timer));

  // Expect that the delete requests are sent with the correct body
  expect_delete("10.0.0.1:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.2:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.3:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.4:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");

  // Expect that the timer is replicated
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsTombstone(), "10.0.0.2:9999")); // Tombstone
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsTombstone(), "10.0.0.4:9999")); // Tombstone
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsNotTombstone(), "10.0.0.3:9999")); // Update

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);

  EXPECT_EQ(200, status);

  delete added_timer; added_timer = NULL;
}

// Test that multiple requests are sent when the response indicates there are
// more timers available. This also checks the time-from parameter.
TEST_F(ChronosInternalConnectionTest, RepeatedTimers)
{
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_2 = new MockHttpRequest();

  HttpResponse resp_partial(HTTP_PARTIAL_CONTENT, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\"], \"Timer\": {\"timing\": { \"start-time-delta\": -235, \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [ \"10.0.0.1:9999\" ] }}}]}", {});
  HttpResponse resp_ok(HTTP_OK, "{\"Timers\":[]}", {});
  HttpResponse accepted(HTTP_ACCEPTED, "", {});

  // Expect that we'll build and send the first resync request with time-from
  // set to 0.
  // Respond with a single timer that has a delta of -235ms, and an interval of
  // 100ms. Set the response code to 206 so that we'll make another request
  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(resp_partial));

  // Expect that we'll build and send the second request with time-from based on
  // the time of the timer we received before.
  // Respond with an empty body as we don't care about any other timers in this
  // test
  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id;time-from=" + std::to_string(100000 - 235 + 1),
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_2));
  EXPECT_CALL(*resync_mock_req_2, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_2, send()).WillOnce(Return(resp_ok));

  // Expect that we'll send deletes to all cluster nodes
  expect_delete("10.0.0.1:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.2:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");
  expect_delete("10.0.0.3:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":0}]}");

  // Save off the added timer so we can delete it (the add_timer call normally
  // deletes the timer but it's mocked out)
  Timer* added_timer;
  EXPECT_CALL(*_th, add_timer(_,_)).WillOnce(SaveArg<0>(&added_timer));

  EXPECT_CALL(*_replicator, replicate_timer_to_node(_, _));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(200, status);

  delete added_timer; added_timer = NULL;
}

TEST_F(ChronosInternalConnectionTest, ResynchronizeWithTimers)
{
  std::vector<std::string> leaving_cluster_addresses;
  leaving_cluster_addresses.push_back("10.0.0.4:9999");

  __globals->set_cluster_leaving_addresses(leaving_cluster_addresses);
  _cluster_addresses.push_back("10.0.0.4:9999");

  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_2 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_3 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_4 = new MockHttpRequest();

  // Timers from 10.0.0.2/10.0.0.3/10.0.0.4 - One timer that's having its replica list reordered.
  // This isn't a valid response (as it should be different for .2/.3/.4), but it's sufficient
  HttpResponse response(HTTP_OK, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.1:9999\", \"10.0.0.2:9999\", \"10.0.0.3:9999\"], \"Timer\": {\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [ \"10.0.0.3:9999\", \"10.0.0.1:9999\", \"10.0.0.2:9999\" ] }}}]}", {});

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.2:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_2));
  EXPECT_CALL(*resync_mock_req_2, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_2, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.3:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_3));
  EXPECT_CALL(*resync_mock_req_3, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_3, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.4:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_4));
  EXPECT_CALL(*resync_mock_req_4, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_4, send()).WillOnce(Return(response));


  // Delete responses - expect them each 4 times as we have the same response
  // from each of the 4 replicas
  for (int i = 0; i < 4; i++)
  {
    expect_delete("10.0.0.1:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":1}]}");
    expect_delete("10.0.0.2:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":1}]}");
    expect_delete("10.0.0.3:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":1}]}");
    expect_delete("10.0.0.4:9999", "{\"IDs\":[{\"ID\":4,\"ReplicaIndex\":1}]}");
  }

  // There should be no calls to add a timer, as the node has moved higher up
  // the replica list
  EXPECT_CALL(*_th, add_timer(_,_)).Times(0);

  // There are no calls to replicate to 10.0.0.3 as it is lower in the replica list
  EXPECT_CALL(*_replicator, replicate_timer_to_node(_, "10.0.0.3:9999")).Times(0);

  // There are four calls to replicate to 10.0.0.2 as it is lower/equal in the
  // old/new replica lists. (Note, you wouldn't expect to call this four times
  // in the real code, this is just because each of the four resync calls returned
  // the same timer).
  EXPECT_CALL(*_replicator, replicate_timer_to_node(IsNotTombstone(), "10.0.0.2:9999")).Times(4);

  _chronos->resynchronize();
}

TEST_F(ChronosInternalConnectionTest, ResynchronizeWithInvalidGetResponse)
{
  // Responses have invalid JSON
  HttpResponse response(HTTP_OK, "{\"Timers\":}", {});

  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_2 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_3 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.2:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_2));
  EXPECT_CALL(*resync_mock_req_2, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_2, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.3:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_3));
  EXPECT_CALL(*resync_mock_req_3, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_3, send()).WillOnce(Return(response));

  // There should be no calls to add/replicate a timer
  EXPECT_CALL(*_th, add_timer(_,_)).Times(0);
  EXPECT_CALL(*_replicator, replicate_timer_to_node(_, _)).Times(0);
  _chronos->resynchronize();
}

TEST_F(ChronosInternalConnectionTest, ResynchronizeWithGetRequestFailed)
{
  // GET requests fail
  HttpResponse response(HTTP_BAD_REQUEST, "", {});

  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_2 = new MockHttpRequest();
  MockHttpRequest* resync_mock_req_3 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.2:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_2));
  EXPECT_CALL(*resync_mock_req_2, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_2, send()).WillOnce(Return(response));

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.3:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_3));
  EXPECT_CALL(*resync_mock_req_3, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_3, send()).WillOnce(Return(response));

  // There should be no calls to add/replicate a timer
  EXPECT_CALL(*_th, add_timer(_,_)).Times(0);
  EXPECT_CALL(*_replicator, replicate_timer_to_node(_, _)).Times(0);
  _chronos->resynchronize();
}

TEST_F(ChronosInternalConnectionTest, SendTriggerInvalidResultsInvalidJSON)
{
  HttpResponse response(HTTP_OK, "{\"Timers\":]}", {});
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(400, status);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerInvalidResultsNoTimers)
{
  HttpResponse response(HTTP_OK, "{\"Timer\":[]}", {});
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(400, status);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerInvalidEntryNoTimerObject)
{
  HttpResponse response(HTTP_OK, "{\"Timers\":[\"Timer\"]}", {});
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(400, status);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerInvalidEntryNoReplicas)
{
  HttpResponse response(HTTP_OK, "{\"Timers\":[{\"TimerID\":4}]}", {});
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(400, status);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerInvalidResultNoTimer)
{
  HttpResponse response(HTTP_OK, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\"]}]}", {});
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(400, status);
}

TEST_F(ChronosInternalConnectionTest, SendTriggerInvalidResultInvalidTimers)
{
  HttpResponse response(HTTP_OK, "{\"Timers\":[{\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\"], \"Timer\": {}}, {\"TimerID\":4, \"OldReplicas\":[\"10.0.0.2:9999\"], \"Timer\": {\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": {}}}]}", {});
  MockHttpRequest* resync_mock_req_1 = new MockHttpRequest();

  EXPECT_CALL(*_chronos, build_request_proxy("10.0.0.1:9999",
                                             "/timers?node-for-replicas=10.0.0.1:9999;cluster-view-id=cluster-view-id",
                                             HttpClient::RequestType::GET))
    .WillOnce(Return(resync_mock_req_1));
  EXPECT_CALL(*resync_mock_req_1, add_header(_)).Times(1);
  EXPECT_CALL(*resync_mock_req_1, send()).WillOnce(Return(response));

  HTTPCode status = _chronos->resynchronise_with_single_node("10.0.0.1:9999", _cluster_addresses, _local_ip);
  EXPECT_EQ(400, status);
}
