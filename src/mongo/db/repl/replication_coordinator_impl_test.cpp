/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_response.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"  // ReplSetReconfigArgs
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

typedef ReplicationCoordinator::ReplSetReconfigArgs ReplSetReconfigArgs;
Status kInterruptedStatus(ErrorCodes::Interrupted, "operation was interrupted");

// Helper class to wrap Timestamp as an OpTime with term 0.
struct OpTimeWithTermZero {
    OpTimeWithTermZero(unsigned int sec, unsigned int i) : timestamp(sec, i) {}
    operator OpTime() const {
        return OpTime(timestamp, 0);
    }

    operator boost::optional<OpTime>() const {
        return OpTime(timestamp, 0);
    }

    Timestamp timestamp;
};

void runSingleNodeElection(ReplicationCoordinatorImpl* replCoord) {
    replCoord->setMyLastOptime(OpTime(Timestamp(1, 0), 0));
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    replCoord->waitForElectionFinish_forTest();

    ASSERT(replCoord->isWaitingForApplierToDrain());
    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();

    OperationContextReplMock txn;
    replCoord->signalDrainComplete(&txn);
}

TEST_F(ReplCoordTest, StartupWithValidLocalConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_TRUE(getExternalState()->threadsStarted());
}

TEST_F(ReplCoordTest, StartupWithValidLocalConfigAsArbiter) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"
                                                     << "arbiterOnly" << true)
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_FALSE(getExternalState()->threadsStarted());
}

TEST_F(ReplCoordTest, StartupWithConfigMissingSelf) {
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:54321"))),
                       HostAndPort("node3", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("NodeNotFound"));
}

TEST_F(ReplCoordTest, StartupWithLocalConfigSetNameMismatch) {
    init("mySet");
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "notMySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("reports set name of notMySet,"));
}

TEST_F(ReplCoordTest, StartupWithNoLocalConfig) {
    startCapturingLogMessages();
    start();
    stopCapturingLogMessages();
    ASSERT_EQUALS(2, countLogLinesContaining("Did not find local "));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithEmptyConfig) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    BSONObjBuilder result;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateSucceedsWithOneNodeConfig) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Starting uninitialized, show that we can perform the initiate behavior.
    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    ASSERT_TRUE(getExternalState()->threadsStarted());

    // Show that initiate fails after it has already succeeded.
    BSONObjBuilder result2;
    ASSERT_EQUALS(
        ErrorCodes::AlreadyInitialized,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result2));

    // Still in repl set mode, even after failed reinitiate.
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, InitiateFailsAsArbiter) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Starting uninitialized, show that we can perform the initiate behavior.
    BSONObjBuilder result1;
    auto status = getReplCoord()->processReplSetInitiate(
        &txn,
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members" << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "node1:12345"
                                                                     << "arbiterOnly" << true)
                                                          << BSON("_id" << 1 << "host"
                                                                        << "node2:12345"))),
        &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "is not electable under the new configuration version");
    ASSERT_FALSE(getExternalState()->threadsStarted());
}

TEST_F(ReplCoordTest, InitiateSucceedsAfterFailing) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    BSONObjBuilder result;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Having failed to initiate once, show that we can now initiate.
    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, InitiateFailsIfAlreadyInitialized) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    BSONObjBuilder result;
    ASSERT_EQUALS(
        ErrorCodes::AlreadyInitialized,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 2 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                             << "node1:12345"))),
                                               &result));
}

TEST_F(ReplCoordTest, InitiateFailsIfSelfMissing) {
    OperationContextNoop txn;
    BSONObjBuilder result;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node4"))),
                                               &result));
}

void doReplSetInitiate(ReplicationCoordinatorImpl* replCoord, Status* status) {
    OperationContextNoop txn;
    BSONObjBuilder garbage;
    *status =
        replCoord->processReplSetInitiate(&txn,
                                          BSON("_id"
                                               << "mySet"
                                               << "version" << 1 << "members"
                                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                        << "node1:12345")
                                                             << BSON("_id" << 1 << "host"
                                                                           << "node2:54321"))),
                                          &garbage);
}

TEST_F(ReplCoordTest, InitiateFailsIfQuorumNotMet) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(1);
    hbArgs.setCheckEmpty(true);
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread(stdx::bind(doReplSetInitiate, getReplCoord(), &status));
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_EQUALS(hbArgs.toBSON(), noi->getRequest().cmdObj);
    getNet()->scheduleResponse(
        noi, startDate + Milliseconds(10), ResponseStatus(ErrorCodes::NoSuchKey, "No response"));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiatePassesIfQuorumMet) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(1);
    hbArgs.setCheckEmpty(true);
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread(stdx::bind(doReplSetInitiate, getReplCoord(), &status));
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_EQUALS(hbArgs.toBSON(), noi->getRequest().cmdObj);
    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(0);
    getNet()->scheduleResponse(
        noi,
        startDate + Milliseconds(10),
        ResponseStatus(RemoteCommandResponse(hbResp.toBSON(false), BSONObj(), Milliseconds(8))));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_OK(status);
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, InitiateFailsWithSetNameMismatch) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "wrongSet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithMissingConfiguration) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status = getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Missing expected field \"_id\"");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithMissingSetName) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status = getReplCoord()->processReplSetInitiate(
        &txn,
        BSON("version" << 1 << "members" << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "node1:12345"))),
        &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Missing expected field \"_id\"");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithIncorrectVersion) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 2 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "have version 1, but found 2");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithMoreThanOneMember) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345")
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "node2:12345"))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "you can only specify one member in the config");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithSelfMissing) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node5:12345"))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "No host described in new configuration");
    ASSERT_STRING_CONTAINS(status.reason(), "maps to this node");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithArbiterMember) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status = getReplCoord()->processReplSetInitiate(
        &txn,
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members" << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "node1:12345"
                                                                     << "arbiterOnly" << true))),
        &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "must contain at least one non-arbiter member");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithPriorityZero) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"
                                                                             << "priority" << 0))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "must contain at least one non-arbiter member");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithNoVotes) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"
                                                                             << "votes" << 0))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "priority must be 0 when non-voting (votes:0)");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlagWithHiddenMember) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"
                                                                             << "hidden" << true))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "priority must be 0 when hidden=true");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiatePassesWithoutReplSetFlagWithValidConfiguration) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
}

TEST_F(ReplCoordTest, InitiateFailsWhileStoringLocalConfigDocument) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    getExternalState()->setStoreLocalConfigDocumentStatus(
        Status(ErrorCodes::OutOfDiskSpace, "The test set this"));
    ASSERT_EQUALS(
        ErrorCodes::OutOfDiskSpace,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, CheckReplEnabledForCommandNotRepl) {
    // pass in settings to avoid having a replSet
    ReplSettings settings;
    init(settings);
    start();

    // check status NoReplicationEnabled and empty result
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, checkReplEnabledForCommandConfigSvr) {
    ReplSettings settings;
    serverGlobalParams.configsvr = true;
    init(settings);
    start();

    // check status NoReplicationEnabled and result mentions configsrv
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
    ASSERT_EQUALS(result.obj()["info"].String(), "configsvr");
    serverGlobalParams.configsvr = false;
}

TEST_F(ReplCoordTest, checkReplEnabledForCommandNoConfig) {
    start();

    // check status NotYetInitialized and result mentions rs.initiate
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NotYetInitialized);
    ASSERT_TRUE(result.obj()["info"].String().find("rs.initiate") != std::string::npos);
}

TEST_F(ReplCoordTest, checkReplEnabledForCommandWorking) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    // check status OK and result is empty
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, Status::OK());
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, BasicRBIDUsage) {
    start();
    BSONObjBuilder result;
    getReplCoord()->processReplSetGetRBID(&result);
    long long initialValue = result.obj()["rbid"].Int();
    getReplCoord()->incrementRollbackID();

    BSONObjBuilder result2;
    getReplCoord()->processReplSetGetRBID(&result2);
    long long incrementedValue = result2.obj()["rbid"].Int();
    ASSERT_EQUALS(incrementedValue, initialValue + 1);
}

TEST_F(ReplCoordTest, AwaitReplicationNoReplEnabled) {
    init("");
    OperationContextNoop txn;
    OpTimeWithTermZero time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 2;

    // Because we didn't set ReplSettings.replSet, it will think we're a standalone so
    // awaitReplication will always work.
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationMasterSlaveMajorityBaseCase) {
    ReplSettings settings;
    settings.master = true;
    init(settings);
    OperationContextNoop txn;
    OpTimeWithTermZero time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 2;


    writeConcern.wNumNodes = 0;
    writeConcern.wMode = WriteConcernOptions::kMajority;
    // w:majority always works on master/slave
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationReplSetBaseCases) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));

    OperationContextNoop txn;
    OpTimeWithTermZero time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 0;  // Waiting for 0 nodes always works
    writeConcern.wMode = "";

    // Should fail when not primary
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_EQUALS(ErrorCodes::NotMaster, statusAndDur.status);

    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    statusAndDur = getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_OK(statusAndDur.status);

    ASSERT_TRUE(getExternalState()->isApplierSignaledToCancelFetcher());
}

TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesNonBlocking) {
    OperationContextNoop txn;
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("host"
                                << "node1:12345"
                                << "_id" << 0)
                           << BSON("host"
                                   << "node2:12345"
                                   << "_id" << 1) << BSON("host"
                                                          << "node3:12345"
                                                          << "_id" << 2) << BSON("host"
                                                                                 << "node4:12345"
                                                                                 << "_id" << 3))),
        HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    // 1 node waiting for time 1
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastOptime(time1);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time1
    writeConcern.wNumNodes = 2;
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time2
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastOptime(time2);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, time2));
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time2));
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationNamedModesNonBlocking) {
    auto service = stdx::make_unique<ServiceContextNoop>();
    auto client = service->makeClient("test");
    OperationContextNoop txn(client.get(), 100);

    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "node0"
                                      << "tags" << BSON("dc"
                                                        << "NA"
                                                        << "rack"
                                                        << "rackNA1"))
                           << BSON("_id" << 1 << "host"
                                         << "node1"
                                         << "tags" << BSON("dc"
                                                           << "NA"
                                                           << "rack"
                                                           << "rackNA2"))
                           << BSON("_id" << 2 << "host"
                                         << "node2"
                                         << "tags" << BSON("dc"
                                                           << "NA"
                                                           << "rack"
                                                           << "rackNA3"))
                           << BSON("_id" << 3 << "host"
                                         << "node3"
                                         << "tags" << BSON("dc"
                                                           << "EU"
                                                           << "rack"
                                                           << "rackEU1"))
                           << BSON("_id" << 4 << "host"
                                         << "node4"
                                         << "tags" << BSON("dc"
                                                           << "EU"
                                                           << "rack"
                                                           << "rackEU2"))) << "settings"
             << BSON("getLastErrorModes" << BSON("multiDC" << BSON("dc" << 2) << "multiDCAndRack"
                                                           << BSON("dc" << 2 << "rack" << 3)))),
        HostAndPort("node0"));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTime(Timestamp(100, 0), 0));
    simulateSuccessfulV1Election();

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);

    // Test invalid write concern
    WriteConcernOptions invalidWriteConcern;
    invalidWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    invalidWriteConcern.wMode = "fakemode";

    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time1, invalidWriteConcern);
    ASSERT_EQUALS(ErrorCodes::UnknownReplWriteConcern, statusAndDur.status);


    // Set up valid write concerns for the rest of the test
    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;

    WriteConcernOptions multiDCWriteConcern;
    multiDCWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiDCWriteConcern.wMode = "multiDC";

    WriteConcernOptions multiRackWriteConcern;
    multiRackWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiRackWriteConcern.wMode = "multiDCAndRack";


    // Nothing satisfied
    getReplCoord()->setMyLastOptime(time1);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);

    // Majority satisfied but not either custom mode
    getReplCoord()->setLastOptime_forTest(2, 1, time1);
    getReplCoord()->setLastOptime_forTest(2, 2, time1);
    getReplCoord()->onSnapshotCreate(time1, SnapshotName(1));

    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);

    // All modes satisfied
    getReplCoord()->setLastOptime_forTest(2, 3, time1);

    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
    ASSERT_OK(statusAndDur.status);

    // Majority also waits for the committed snapshot to be newer than all snapshots reserved by
    // this operation. Custom modes not affected by this.
    while (getReplCoord()->reserveSnapshotName(&txn) <= SnapshotName(1)) {
        // These unittests "cheat" and use SnapshotName(1) without advancing the counter. Reserve
        // another name if we didn't get a high enough one.
    }

    statusAndDur = getReplCoord()->awaitReplicationOfLastOpForClient(&txn, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplicationOfLastOpForClient(&txn, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplicationOfLastOpForClient(&txn, multiRackWriteConcern);
    ASSERT_OK(statusAndDur.status);

    // All modes satisfied
    getReplCoord()->onSnapshotCreate(time1, getReplCoord()->reserveSnapshotName(nullptr));

    statusAndDur = getReplCoord()->awaitReplicationOfLastOpForClient(&txn, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplicationOfLastOpForClient(&txn, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplicationOfLastOpForClient(&txn, multiRackWriteConcern);
    ASSERT_OK(statusAndDur.status);

    // multiDC satisfied but not majority or multiRack
    getReplCoord()->setMyLastOptime(time2);
    getReplCoord()->setLastOptime_forTest(2, 3, time2);

    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
}

/**
 * Used to wait for replication in a separate thread without blocking execution of the test.
 * To use, set the optime and write concern to be passed to awaitReplication and then call
 * start(), which will spawn a thread that calls awaitReplication.  No calls may be made
 * on the ReplicationAwaiter instance between calling start and getResult().  After returning
 * from getResult(), you can call reset() to allow the awaiter to be reused for another
 * awaitReplication call.
 */
class ReplicationAwaiter {
public:
    ReplicationAwaiter(ReplicationCoordinatorImpl* replCoord, OperationContext* txn)
        : _replCoord(replCoord),
          _finished(false),
          _result(ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0))) {}

    void setOpTime(const OpTime& ot) {
        _optime = ot;
    }

    void setWriteConcern(const WriteConcernOptions& wc) {
        _writeConcern = wc;
    }

    // may block
    ReplicationCoordinator::StatusAndDuration getResult() {
        _thread->join();
        ASSERT(_finished);
        return _result;
    }

    void start(OperationContext* txn) {
        ASSERT(!_finished);
        _thread.reset(
            new stdx::thread(stdx::bind(&ReplicationAwaiter::_awaitReplication, this, txn)));
    }

    void reset() {
        ASSERT(_finished);
        _finished = false;
        _result = ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
    }

private:
    void _awaitReplication(OperationContext* txn) {
        _result = _replCoord->awaitReplication(txn, _optime, _writeConcern);
        _finished = true;
    }

    ReplicationCoordinatorImpl* _replCoord;
    bool _finished;
    OpTime _optime;
    WriteConcernOptions _writeConcern;
    ReplicationCoordinator::StatusAndDuration _result;
    std::unique_ptr<stdx::thread> _thread;
};

TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesBlocking) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time1
    awaiter.setOpTime(time1);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    getReplCoord()->setMyLastOptime(time1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.start(&txn);
    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationTimeout) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = 50;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationShutdown) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time1));
    shutdown();
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationStepDown) {
    // Test that a thread blocked in awaitReplication will be woken up and return NotMaster
    // if the node steps down while it is waiting.
    OperationContextReplMock txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time1));
    getReplCoord()->stepDown(&txn, true, Milliseconds(0), Milliseconds(1000));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::NotMaster, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationInterrupt) {
    // Tests that a thread blocked in awaitReplication can be killed by a killOp operation
    const unsigned int opID = 100;
    OperationContextReplMock txn{opID};
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node1")
                                          << BSON("_id" << 1 << "host"
                                                        << "node2") << BSON("_id" << 2 << "host"
                                                                                  << "node3"))),
                       HostAndPort("node1"));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;


    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time1));

    txn.setCheckForInterruptStatus(kInterruptedStatus);
    getReplCoord()->interrupt(opID);
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::Interrupted, statusAndDur.status);
    awaiter.reset();
}

class StepDownTest : public ReplCoordTest {
protected:
    OID myRid;
    OID rid2;
    OID rid3;

private:
    virtual void setUp() {
        ReplCoordTest::setUp();
        init("mySet/test1:1234,test2:1234,test3:1234");

        assertStartSuccess(BSON("_id"
                                << "mySet"
                                << "version" << 1 << "members"
                                << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                         << "test1:1234")
                                              << BSON("_id" << 1 << "host"
                                                            << "test2:1234")
                                              << BSON("_id" << 2 << "host"
                                                            << "test3:1234"))),
                           HostAndPort("test1", 1234));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        myRid = getReplCoord()->getMyRID();
    }
};

TEST_F(ReplCoordTest, UpdateTermNotReplMode) {
    init(ReplSettings());
    ASSERT_TRUE(ReplicationCoordinator::modeNone == getReplCoord()->getReplicationMode());
    ASSERT_EQUALS(ErrorCodes::BadValue, getReplCoord()->updateTerm(0).code());
}

TEST_F(ReplCoordTest, UpdateTerm) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))
             << "protocolVersion" << 1),
        HostAndPort("test1", 1234));
    getReplCoord()->setMyLastOptime(OpTime(Timestamp(100, 1), 0));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1Election();

    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // lower term, no change
    ASSERT_OK(getReplCoord()->updateTerm(0));
    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // same term, no change
    ASSERT_OK(getReplCoord()->updateTerm(1));
    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // higher term, step down and change term
    Handle cbHandle;
    ASSERT_EQUALS(ErrorCodes::StaleTerm, getReplCoord()->updateTerm(2).code());
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
    getReplCoord()->waitForStepDownFinish_forTest();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownNotPrimary) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    Status status = getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(0));
    ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownTimeoutAcquiringGlobalLock) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    // Make sure stepDown cannot grab the global shared lock
    Lock::GlobalWrite lk(txn.lockState());

    Status status = getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(StepDownTest, StepDownNoWaiting) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    enterNetwork();
    getNet()->runUntil(getNet()->now() + Seconds(2));
    ASSERT(getNet()->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    log() << request.target.toString() << " processing " << request.cmdObj;
    ReplSetHeartbeatArgsV1 hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        hbResp.setOpTime(optime1);
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj, false);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
    }
    while (getNet()->hasReadyRequests()) {
        getNet()->blackHole(getNet()->getNextReadyRequest());
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();


    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT_OK(getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000)));
    enterNetwork();  // So we can safely inspect the topology coordinator
    ASSERT_EQUALS(getNet()->now() + Seconds(1), getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, StepDownAndBackUpSingleNode) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    OperationContextReplMock txn;
    runSingleNodeElection(getReplCoord());
    ASSERT_OK(getReplCoord()->stepDown(&txn, true, Milliseconds(0), Milliseconds(1000)));
    getNet()->enterNetwork();  // Must do this before inspecting the topocoord
    Date_t stepdownUntil = getNet()->now() + Seconds(1);
    ASSERT_EQUALS(stepdownUntil, getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Now run time forward and make sure that the node becomes primary again when the stepdown
    // period ends.
    getNet()->runUntil(stepdownUntil);
    ASSERT_EQUALS(stepdownUntil, getNet()->now());
    ASSERT_TRUE(getTopoCoord().getMemberState().primary());
    getNet()->exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(StepDownTest, StepDownNotCaughtUp) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastOptime(optime2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    // Try to stepDown but time out because no secondaries are caught up.
    auto status = repl->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(repl->getMemberState().primary());

    // Now use "force" to force it to step down even though no one is caught up
    getNet()->enterNetwork();
    const Date_t startDate = getNet()->now();
    while (startDate + Milliseconds(1000) < getNet()->now()) {
        while (getNet()->hasReadyRequests()) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
        getNet()->runUntil(startDate + Milliseconds(1000));
    }
    getNet()->exitNetwork();
    ASSERT_TRUE(repl->getMemberState().primary());
    status = repl->stepDown(&txn, true, Milliseconds(0), Milliseconds(1000));
    ASSERT_OK(status);
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownCatchUp) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastOptime(optime2);
    ASSERT_OK(repl->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.
    Status result(ErrorCodes::InternalError, "not mutated");
    auto globalReadLockAndEventHandle =
        repl->stepDown_nonBlocking(&txn, false, Milliseconds(10000), Milliseconds(60000), &result);
    const auto& eventHandle = globalReadLockAndEventHandle.second;
    ASSERT_TRUE(eventHandle);
    ASSERT_TRUE(txn.lockState()->isReadLocked());

    // Make a secondary actually catch up
    enterNetwork();
    ASSERT(getNet()->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    log() << request.target.toString() << " processing " << request.cmdObj;
    ReplSetHeartbeatArgsV1 hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        hbResp.setOpTime(optime2);
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj, false);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
    }
    while (getNet()->hasReadyRequests()) {
        auto noi = getNet()->getNextReadyRequest();
        log() << "Blackholing network request " << noi->getRequest().cmdObj;
        getNet()->blackHole(noi);
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    getReplExec()->waitForEvent(eventHandle);
    ASSERT_OK(result);
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownCatchUpOnSecondHeartbeat) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastOptime(optime2);
    ASSERT_OK(repl->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.
    Status result(ErrorCodes::InternalError, "not mutated");
    auto globalReadLockAndEventHandle =
        repl->stepDown_nonBlocking(&txn, false, Milliseconds(10000), Milliseconds(60000), &result);
    const auto& eventHandle = globalReadLockAndEventHandle.second;
    ASSERT_TRUE(eventHandle);
    ASSERT_TRUE(txn.lockState()->isReadLocked());

    // Secondary has not caught up on first round of heartbeats.
    enterNetwork();
    ASSERT(getNet()->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    log() << "HB1: " << request.target.toString() << " processing " << request.cmdObj;
    ReplSetHeartbeatArgsV1 hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj, false);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
    }
    while (getNet()->hasReadyRequests()) {
        getNet()->blackHole(getNet()->getNextReadyRequest());
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    auto config = getReplCoord()->getConfig();
    auto heartbeatInterval = config.getHeartbeatInterval();

    // Make a secondary actually catch up
    enterNetwork();
    auto until = getNet()->now() + heartbeatInterval;
    getNet()->runUntil(until);
    ASSERT_EQUALS(until, getNet()->now());
    ASSERT(getNet()->hasReadyRequests());
    noi = getNet()->getNextReadyRequest();
    request = noi->getRequest();
    log() << "HB2: " << request.target.toString() << " processing " << request.cmdObj;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        hbResp.setOpTime(optime2);
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj, false);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
    }
    while (getNet()->hasReadyRequests()) {
        getNet()->blackHole(getNet()->getNextReadyRequest());
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    getReplExec()->waitForEvent(eventHandle);
    ASSERT_OK(result);
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest, InterruptStepDown) {
    const unsigned int opID = 100;
    OperationContextReplMock txn{opID};
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastOptime(optime2);
    ASSERT_OK(repl->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(repl->getMemberState().primary());

    // stepDown where the secondary actually has to catch up before the stepDown can succeed.
    Status result(ErrorCodes::InternalError, "not mutated");
    auto globalReadLockAndEventHandle =
        repl->stepDown_nonBlocking(&txn, false, Milliseconds(10000), Milliseconds(60000), &result);
    const auto& eventHandle = globalReadLockAndEventHandle.second;
    ASSERT_TRUE(eventHandle);
    ASSERT_TRUE(txn.lockState()->isReadLocked());

    txn.setCheckForInterruptStatus(kInterruptedStatus);
    getReplCoord()->interrupt(opID);

    getReplExec()->waitForEvent(eventHandle);
    ASSERT_EQUALS(ErrorCodes::Interrupted, result);
    ASSERT_TRUE(repl->getMemberState().primary());
}

TEST_F(ReplCoordTest, GetReplicationModeNone) {
    init();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, GetReplicationModeMaster) {
    // modeMasterSlave if master set
    ReplSettings settings;
    settings.master = true;
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, GetReplicationModeSlave) {
    // modeMasterSlave if the slave flag was set
    ReplSettings settings;
    settings.slave = SimpleSlave;
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, GetReplicationModeRepl) {
    // modeReplSet if the set name was supplied.
    ReplSettings settings;
    settings.replSet = "mySet/node1:12345";
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
}

TEST_F(ReplCoordTest, TestPrepareReplSetUpdatePositionCommand) {
    OperationContextNoop txn;
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))),
        HostAndPort("test1", 1234));
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    OpTimeWithTermZero optime3(2, 1);
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime2));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime3));

    // Check that the proper BSON is generated for the replSetUpdatePositionCommand
    BSONObjBuilder cmdBuilder;
    getReplCoord()->prepareReplSetUpdatePositionCommand(&cmdBuilder);
    BSONObj cmd = cmdBuilder.done();

    ASSERT_EQUALS(2, cmd.nFields());
    ASSERT_EQUALS("replSetUpdatePosition", cmd.firstElement().fieldNameStringData());

    std::set<long long> memberIds;
    BSONForEach(entryElement, cmd["optimes"].Obj()) {
        BSONObj entry = entryElement.Obj();
        long long memberId = entry["memberId"].Number();
        memberIds.insert(memberId);
        if (memberId == 0) {
            // TODO(siyuan) Update when we change replSetUpdatePosition format
            ASSERT_EQUALS(optime1.timestamp, entry["optime"]["ts"].timestamp());
        } else if (memberId == 1) {
            ASSERT_EQUALS(optime2.timestamp, entry["optime"]["ts"].timestamp());
        } else {
            ASSERT_EQUALS(2, memberId);
            ASSERT_EQUALS(optime3.timestamp, entry["optime"]["ts"].timestamp());
        }
        ASSERT_EQUALS(0, entry["optime"]["t"].Number());
    }
    ASSERT_EQUALS(3U, memberIds.size());  // Make sure we saw all 3 nodes
}

TEST_F(ReplCoordTest, SetMaintenanceMode) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "protocolVersion" << 1 << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))),
        HostAndPort("test2", 1234));
    OperationContextNoop txn;
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));

    // Can't unset maintenance mode if it was never set to begin with.
    Status status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // valid set
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());

    // If we go into rollback while in maintenance mode, our state changes to RS_ROLLBACK.
    getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // When we go back to SECONDARY, we still observe RECOVERING because of maintenance mode.
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());

    // Can set multiple times
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));

    // Need to unset the number of times you set
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    status = getReplCoord()->setMaintenanceMode(false);
    // fourth one fails b/c we only set three times
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    // Unsetting maintenance mode changes our state to secondary if maintenance mode was
    // the only thinking keeping us out of it.
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // From rollback, entering and exiting maintenance mode doesn't change perceived
    // state.
    getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // Rollback is sticky even if entered while in maintenance mode.
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());
    getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Can't modify maintenance mode when PRIMARY
    simulateSuccessfulV1Election();

    status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down from primary.
    getReplCoord()->updateTerm(getReplCoord()->getTerm() + 1);
    getReplCoord()->waitForMemberState_forTest(MemberState::RS_SECONDARY);

    status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));

    // Can't modify maintenance mode when running for election (before and after dry run).
    ASSERT_EQUALS(TopologyCoordinator::Role::follower, getTopoCoord().getRole());
    auto net = this->getNet();
    net->enterNetwork();
    auto when = getReplCoord()->getElectionTimeout_forTest();
    while (net->now() < when) {
        net->runUntil(when);
        if (!net->hasReadyRequests()) {
            continue;
        }
        net->blackHole(net->getNextReadyRequest());
    }
    ASSERT_EQUALS(when, net->now());
    net->exitNetwork();
    ASSERT_EQUALS(TopologyCoordinator::Role::candidate, getTopoCoord().getRole());
    status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);

    simulateSuccessfulDryRun();
    ASSERT_EQUALS(TopologyCoordinator::Role::candidate, getTopoCoord().getRole());
    status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);

    // This cancels the actual election.
    bool success = false;
    auto event = getReplCoord()->setFollowerMode_nonBlocking(MemberState::RS_ROLLBACK, &success);
    // We do not need to respond to any pending network operations because setFollowerMode() will
    // cancel the vote requester.
    getReplCoord()->waitForElectionFinish_forTest();
    getReplExec()->waitForEvent(event);
    ASSERT_TRUE(success);
}

TEST_F(ReplCoordTest, GetHostsWrittenToReplSet) {
    HostAndPort myHost("node1:12345");
    HostAndPort client1Host("node2:12345");
    HostAndPort client2Host("node3:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host" << myHost.toString())
                                          << BSON("_id" << 1 << "host" << client1Host.toString())
                                          << BSON("_id" << 2 << "host" << client2Host.toString()))),
                       HostAndPort("node1", 12345));
    OperationContextNoop txn;

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));

    std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(myHost, caughtUpHosts[0]);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time2));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(2U, caughtUpHosts.size());
    if (myHost == caughtUpHosts[0]) {
        ASSERT_EQUALS(client2Host, caughtUpHosts[1]);
    } else {
        ASSERT_EQUALS(client2Host, caughtUpHosts[0]);
        ASSERT_EQUALS(myHost, caughtUpHosts[1]);
    }
}

TEST_F(ReplCoordTest, GetHostsWrittenToMasterSlave) {
    ReplSettings settings;
    settings.master = true;
    init(settings);
    HostAndPort clientHost("node2:12345");
    OperationContextNoop txn;

    OID client = OID::gen();
    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    getExternalState()->setClientHostAndPort(clientHost);
    HandshakeArgs handshake;
    ASSERT_OK(handshake.initialize(BSON("handshake" << client)));
    ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake));

    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptimeForSlave(client, time1.timestamp));

    std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(0U, caughtUpHosts.size());  // self doesn't get included in master-slave

    ASSERT_OK(getReplCoord()->setLastOptimeForSlave(client, time2.timestamp));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(clientHost, caughtUpHosts[0]);
}

TEST_F(ReplCoordTest, GetOtherNodesInReplSetNoConfig) {
    start();
    ASSERT_EQUALS(0U, getReplCoord()->getOtherNodesInReplSet().size());
}

TEST_F(ReplCoordTest, GetOtherNodesInReplSet) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "h1")
                                          << BSON("_id" << 1 << "host"
                                                        << "h2")
                                          << BSON("_id" << 2 << "host"
                                                        << "h3"
                                                        << "priority" << 0 << "hidden" << true))),
                       HostAndPort("h1"));

    std::vector<HostAndPort> otherNodes = getReplCoord()->getOtherNodesInReplSet();
    ASSERT_EQUALS(2U, otherNodes.size());
    if (otherNodes[0] == HostAndPort("h2")) {
        ASSERT_EQUALS(HostAndPort("h3"), otherNodes[1]);
    } else {
        ASSERT_EQUALS(HostAndPort("h3"), otherNodes[0]);
        ASSERT_EQUALS(HostAndPort("h2"), otherNodes[0]);
    }
}

TEST_F(ReplCoordTest, IsMasterNoConfig) {
    start();
    IsMasterResponse response;

    getReplCoord()->fillIsMasterForReplSet(&response);
    ASSERT_FALSE(response.isConfigSet());
    BSONObj responseObj = response.toBSON();
    ASSERT_FALSE(responseObj["ismaster"].Bool());
    ASSERT_FALSE(responseObj["secondary"].Bool());
    ASSERT_TRUE(responseObj["isreplicaset"].Bool());
    ASSERT_EQUALS("Does not have a valid replica set config", responseObj["info"].String());

    IsMasterResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response.toBSON()));
}

TEST_F(ReplCoordTest, IsMaster) {
    HostAndPort h1("h1");
    HostAndPort h2("h2");
    HostAndPort h3("h3");
    HostAndPort h4("h4");
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host" << h1.toString())
                           << BSON("_id" << 1 << "host" << h2.toString())
                           << BSON("_id" << 2 << "host" << h3.toString() << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host" << h4.toString() << "priority" << 0
                                         << "tags" << BSON("key1"
                                                           << "value1"
                                                           << "key2"
                                                           << "value2")))),
        h4);
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    IsMasterResponse response;
    getReplCoord()->fillIsMasterForReplSet(&response);

    ASSERT_EQUALS("mySet", response.getReplSetName());
    ASSERT_EQUALS(2, response.getReplSetVersion());
    ASSERT_FALSE(response.isMaster());
    ASSERT_TRUE(response.isSecondary());
    // TODO(spencer): test that response includes current primary when there is one.
    ASSERT_FALSE(response.isArbiterOnly());
    ASSERT_TRUE(response.isPassive());
    ASSERT_FALSE(response.isHidden());
    ASSERT_TRUE(response.shouldBuildIndexes());
    ASSERT_EQUALS(Seconds(0), response.getSlaveDelay());
    ASSERT_EQUALS(h4, response.getMe());

    std::vector<HostAndPort> hosts = response.getHosts();
    ASSERT_EQUALS(2U, hosts.size());
    if (hosts[0] == h1) {
        ASSERT_EQUALS(h2, hosts[1]);
    } else {
        ASSERT_EQUALS(h2, hosts[0]);
        ASSERT_EQUALS(h1, hosts[1]);
    }
    std::vector<HostAndPort> passives = response.getPassives();
    ASSERT_EQUALS(1U, passives.size());
    ASSERT_EQUALS(h4, passives[0]);
    std::vector<HostAndPort> arbiters = response.getArbiters();
    ASSERT_EQUALS(1U, arbiters.size());
    ASSERT_EQUALS(h3, arbiters[0]);

    unordered_map<std::string, std::string> tags = response.getTags();
    ASSERT_EQUALS(2U, tags.size());
    ASSERT_EQUALS("value1", tags["key1"]);
    ASSERT_EQUALS("value2", tags["key2"]);

    IsMasterResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response.toBSON()));
}

TEST_F(ReplCoordTest, ShutDownBeforeStartUpFinished) {
    init();
    startCapturingLogMessages();
    getReplCoord()->shutdown();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("shutdown() called before startReplication() finished"));
}

TEST_F(ReplCoordTest, UpdatePositionWithConfigVersionAndMemberIdTest) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulV1Election();

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);
    OpTimeWithTermZero staleTime(10, 0);
    getReplCoord()->setMyLastOptime(time1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive updatePosition containing ourself, should not process the update for self
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON("replSetUpdatePosition"
                                   << 1 << "optimes"
                                   << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 0 << "optime"
                                                               << time2.timestamp)))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args, 0));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive updatePosition with incorrect config version
    UpdatePositionArgs args2;
    ASSERT_OK(args2.initialize(BSON("replSetUpdatePosition"
                                    << 1 << "optimes"
                                    << BSON_ARRAY(BSON("cfgver" << 3 << "memberId" << 1 << "optime"
                                                                << time2.timestamp)))));

    long long cfgver;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetUpdatePosition(args2, &cfgver));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive updatePosition with nonexistent member id
    UpdatePositionArgs args3;
    ASSERT_OK(args3.initialize(BSON("replSetUpdatePosition"
                                    << 1 << "optimes"
                                    << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 9 << "optime"
                                                                << time2.timestamp)))));

    ASSERT_EQUALS(ErrorCodes::NodeNotFound, getReplCoord()->processReplSetUpdatePosition(args3, 0));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive a good update position
    getReplCoord()->setMyLastOptime(time2);
    UpdatePositionArgs args4;
    ASSERT_OK(args4.initialize(
        BSON("replSetUpdatePosition"
             << 1 << "optimes"
             << BSON_ARRAY(
                    BSON("cfgver" << 2 << "memberId" << 1 << "optime" << time2.timestamp)
                    << BSON("cfgver" << 2 << "memberId" << 2 << "optime" << time2.timestamp)))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args4, 0));
    ASSERT_OK(getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    writeConcern.wNumNodes = 3;
    ASSERT_OK(getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);
}

void doReplSetReconfig(ReplicationCoordinatorImpl* replCoord, Status* status) {
    OperationContextNoop txn;
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 3)
                                           << BSON("_id" << 1 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(&txn, args, &garbage);
}

TEST_F(ReplCoordTest, AwaitReplicationReconfigSimple) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 2));
    simulateSuccessfulV1Election();

    OpTimeWithTermZero time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 3;

    ReplicationAwaiter awaiter(getReplCoord(), &txn);
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);

    // reconfig
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

    replyToReceivedHeartbeat();
    reconfigThread.join();
    ASSERT_OK(status);

    // satisfy write concern
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(3, 0, time));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(3, 1, time));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(3, 2, time));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

void doReplSetReconfigToFewer(ReplicationCoordinatorImpl* replCoord, Status* status) {
    OperationContextNoop txn;
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(&txn, args, &garbage);
}

TEST_F(ReplCoordTest, AwaitReplicationReconfigNodeCountExceedsNumberOfNodes) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 2));
    simulateSuccessfulV1Election();

    OpTimeWithTermZero time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 3;

    ReplicationAwaiter awaiter(getReplCoord(), &txn);
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);

    // reconfig to fewer nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(stdx::bind(doReplSetReconfigToFewer, getReplCoord(), &status));

    replyToReceivedHeartbeat();

    reconfigThread.join();
    ASSERT_OK(status);

    // writeconcern feasability should be reevaluated and an error should be returned
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationReconfigToSmallerMajority) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3) << BSON("host"
                                                                         << "node5:12345"
                                                                         << "_id" << 4))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 1));
    simulateSuccessfulV1Election();

    OpTime time(Timestamp(100, 2), 1);

    getReplCoord()->setMyLastOptime(time);
    getReplCoord()->onSnapshotCreate(time, SnapshotName(1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time));


    // majority nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wMode = WriteConcernOptions::kMajority;

    ReplicationAwaiter awaiter(getReplCoord(), &txn);
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);

    // demonstrate that majority cannot currently be satisfied
    WriteConcernOptions writeConcern2;
    writeConcern2.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern2.wMode = WriteConcernOptions::kMajority;
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, writeConcern2).status);

    // reconfig to three nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

    replyToReceivedHeartbeat();
    reconfigThread.join();
    ASSERT_OK(status);

    // writeconcern feasability should be reevaluated and be satisfied
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationMajority) {
    // Test that we can satisfy majority write concern can only be
    // statisfied by voting data-bearing members.
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3 << "votes" << 0 << "priority" << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime time(Timestamp(100, 0), 1);
    getReplCoord()->setMyLastOptime(time);
    simulateSuccessfulV1Election();

    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    // this member does not vote and as a result should not count towards write concern
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    getReplCoord()->onSnapshotCreate(time, SnapshotName(1));
    ASSERT_OK(getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);
}

TEST_F(ReplCoordTest, LastCommittedOpTime) {
    // Test that the commit level advances properly.
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3 << "votes" << 0 << "priority" << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime zero(Timestamp(0, 0), 0);
    OpTime time(Timestamp(100, 0), 1);
    getReplCoord()->setMyLastOptime(time);
    simulateSuccessfulV1Election();
    ASSERT_EQUALS(zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time));
    ASSERT_EQUALS(zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, time));
    ASSERT_EQUALS(zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time));
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());


    // Set a new, later OpTime.
    OpTime newTime(Timestamp(100, 1), 1);
    getReplCoord()->setMyLastOptime(newTime);
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, newTime));
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, newTime));
    // Reached majority of voting nodes with newTime.
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, newTime));
    ASSERT_EQUALS(newTime, getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest, CantUseReadAfterIfNotReplSet) {
    init(ReplSettings());
    OperationContextNoop txn;
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTimeWithTermZero(50, 0), ReadConcernLevel::kLocalReadConcern));

    ASSERT_FALSE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::NotAReplicaSet, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterWhileShutdown) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(10, 0));

    shutdown();

    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTimeWithTermZero(50, 0), ReadConcernLevel::kLocalReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterInterrupted) {
    OperationContextReplMock txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(10, 0));

    txn.setCheckForInterruptStatus(Status(ErrorCodes::Interrupted, "test"));

    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTimeWithTermZero(50, 0), ReadConcernLevel::kLocalReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::Interrupted, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterNoOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    auto result = getReplCoord()->waitUntilOpTime(&txn, ReadConcernArgs());

    ASSERT(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterGreaterOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTimeWithTermZero(50, 0), ReadConcernLevel::kLocalReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterEqualOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTimeWithTermZero time(100, 0);
    getReplCoord()->setMyLastOptime(time);
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(time, ReadConcernLevel::kLocalReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, CantUseReadAfterCommittedIfNotReplSet) {
    auto settings = ReplSettings();
    settings.majorityReadConcernEnabled = true;
    init(settings);

    OperationContextNoop txn;
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));

    ASSERT_FALSE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::NotAReplicaSet, result.getStatus());
}

TEST_F(ReplCoordTest, CantUseReadAfterCommittedIfNotEnabled) {
    init(ReplSettings());
    OperationContextNoop txn;
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));

    ASSERT_FALSE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::ReadConcernMajorityNotEnabled, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterCommittedWhileShutdown) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
    runSingleNodeElection(getReplCoord());

    getReplCoord()->setMyLastOptime(OpTime(Timestamp(10, 0), 0));

    shutdown();

    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterCommittedInterrupted) {
    OperationContextReplMock txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
    runSingleNodeElection(getReplCoord());

    getReplCoord()->setMyLastOptime(OpTime(Timestamp(10, 0), 0));

    txn.setCheckForInterruptStatus(Status(ErrorCodes::Interrupted, "test"));

    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::Interrupted, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterCommittedGreaterOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
    runSingleNodeElection(getReplCoord());

    getReplCoord()->setMyLastOptime(OpTime(Timestamp(100, 0), 1));
    getReplCoord()->onSnapshotCreate(OpTime(Timestamp(100, 0), 1), SnapshotName(1));
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(OpTime(Timestamp(50, 0), 1), ReadConcernLevel::kMajorityReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterCommittedEqualOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
    runSingleNodeElection(getReplCoord());
    OpTime time(Timestamp(100, 0), 1);
    getReplCoord()->setMyLastOptime(time);
    getReplCoord()->onSnapshotCreate(time, SnapshotName(1));
    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(time, ReadConcernLevel::kMajorityReadConcern));

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterCommittedDeferredGreaterOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
    runSingleNodeElection(getReplCoord());
    getReplCoord()->setMyLastOptime(OpTime(Timestamp(0, 0), 1));
    OpTime committedOpTime(Timestamp(200, 0), 1);
    auto pseudoLogOp =
        stdx::async(stdx::launch::async,
                    [this, &committedOpTime]() {
                        // Not guaranteed to be scheduled after waitUntil blocks...
                        getReplCoord()->setMyLastOptime(committedOpTime);
                        getReplCoord()->onSnapshotCreate(committedOpTime, SnapshotName(1));
                    });

    auto result = getReplCoord()->waitUntilOpTime(
        &txn,
        ReadConcernArgs(OpTime(Timestamp(100, 0), 1), ReadConcernLevel::kMajorityReadConcern));
    pseudoLogOp.get();

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterCommittedDeferredEqualOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
    runSingleNodeElection(getReplCoord());
    getReplCoord()->setMyLastOptime(OpTime(Timestamp(0, 0), 1));

    OpTime opTimeToWait(Timestamp(100, 0), 1);

    auto pseudoLogOp =
        stdx::async(stdx::launch::async,
                    [this, &opTimeToWait]() {
                        // Not guaranteed to be scheduled after waitUntil blocks...
                        getReplCoord()->setMyLastOptime(opTimeToWait);
                        getReplCoord()->onSnapshotCreate(opTimeToWait, SnapshotName(1));
                    });

    auto result = getReplCoord()->waitUntilOpTime(
        &txn, ReadConcernArgs(opTimeToWait, ReadConcernLevel::kMajorityReadConcern));
    pseudoLogOp.get();

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, MetadataWrongConfigVersion) {
    // Ensure that we do not process ReplSetMetadata when ConfigVersions do not match.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // lower configVersion
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 2) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 0) << "t" << 2) << "configVersion" << 1
                              << "primaryIndex" << 2 << "term" << 2 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // higher configVersion
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(
        BSON(rpc::kReplSetMetadataFieldName
             << BSON("lastOpCommitted"
                     << BSON("ts" << Timestamp(10, 0) << "t" << 2) << "lastOpVisible"
                     << BSON("ts" << Timestamp(10, 0) << "t" << 2) << "configVersion" << 100
                     << "primaryIndex" << 2 << "term" << 2 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest, MetadataUpdatesLastCommittedOpTime) {
    // Ensure that LastCommittedOpTime updates when a newer OpTime comes in via ReplSetMetadata,
    // but not if the OpTime is older than the current LastCommittedOpTime.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    getReplCoord()->updateTerm(1);
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    OpTime time(Timestamp(10, 0), 1);
    getReplCoord()->onSnapshotCreate(time, SnapshotName(1));

    // higher OpTime, should change
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 1) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 0) << "t" << 1) << "configVersion" << 2
                              << "primaryIndex" << 2 << "term" << 1 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(10, 0), 1), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(OpTime(Timestamp(10, 0), 1), getReplCoord()->getCurrentCommittedSnapshotOpTime());

    // lower OpTime, should not change
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(9, 0) << "t" << 1) << "lastOpVisible"
                              << BSON("ts" << Timestamp(9, 0) << "t" << 1) << "configVersion" << 2
                              << "primaryIndex" << 2 << "term" << 1 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(10, 0), 1), getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest, MetadataUpdatesTermAndPrimaryId) {
    // Ensure that the term is updated if and only if the term is greater than our current term.
    // Ensure that currentPrimaryIndex is never altered by ReplSetMetadata.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    getReplCoord()->updateTerm(1);
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    // higher term, should change
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 3) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 0) << "t" << 3) << "configVersion" << 2
                              << "primaryIndex" << 2 << "term" << 3 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(10, 0), 3), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());

    // lower term, should not change
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(11, 0) << "t" << 3) << "lastOpVisible"
                              << BSON("ts" << Timestamp(11, 0) << "t" << 3) << "configVersion" << 2
                              << "primaryIndex" << 1 << "term" << 2 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(11, 0), 3), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());

    // same term, should not change
    StatusWith<rpc::ReplSetMetadata> metadata3 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(11, 0) << "t" << 3) << "lastOpVisible"
                              << BSON("ts" << Timestamp(11, 0) << "t" << 3) << "configVersion" << 2
                              << "primaryIndex" << 1 << "term" << 3 << "syncSourceIndex" << 1)));
    getReplCoord()->processReplSetMetadata(metadata3.getValue());
    ASSERT_EQUALS(OpTime(Timestamp(11, 0), 3), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
}

TEST_F(ReplCoordTest, CancelAndRescheduleElectionTimeout) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();


    auto net = getNet();
    net->enterNetwork();

    // Black hole heartbeat request scheduled after transitioning to SECONDARY.
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    const auto& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());
    log() << "black holing " << noi->getRequest().cmdObj;
    net->blackHole(noi);

    // Advance simulator clock to some time before the first scheduled election.
    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    log() << "Election initially scheduled at " << electionTimeoutWhen << " (simulator time)";
    ASSERT_GREATER_THAN(electionTimeoutWhen, net->now());
    auto until = net->now() + (electionTimeoutWhen - net->now()) / 2;
    net->runUntil(until);
    ASSERT_EQUALS(until, net->now());
    net->exitNetwork();

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    ASSERT_LESS_THAN_OR_EQUALS(until + replCoord->getConfig().getElectionTimeoutPeriod(),
                               replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest, CancelAndRescheduleElectionTimeoutWhenNotProtocolVersion1) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 0 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest, CancelAndRescheduleElectionTimeoutWhenNotSecondary) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_ROLLBACK));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest, CancelAndRescheduleElectionTimeoutWhenNotElectable) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0 << "priority" << 0 << "hidden" << true)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest, CancelAndRescheduleElectionTimeoutWhenRemovedDueToReconfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    log() << "processing " << request.cmdObj;
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command with a config that excludes node1.
    ReplSetHeartbeatResponse hbResp;
    ReplicaSetConfig config;
    config.initialize(BSON("_id"
                           << "mySet"
                           << "protocolVersion" << 1 << "version" << 3 << "members"
                           << BSON_ARRAY(BSON("host"
                                              << "node2:12345"
                                              << "_id" << 1))));
    hbResp.setConfig(config);
    hbResp.setConfigVersion(3);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true)));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    getReplCoord()->waitForMemberState_forTest(MemberState::RS_REMOVED);
    ASSERT_EQUALS(config.getConfigVersion(), getReplCoord()->getConfig().getConfigVersion());

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    ASSERT_EQUALS(Date_t(), replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       CancelAndRescheduleElectionTimeoutWhenProcessingHeartbeatResponseFromPrimary) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    log() << "processing " << request.cmdObj;
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON(true)));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_LESS_THAN_OR_EQUALS(heartbeatWhen + replCoord->getConfig().getElectionTimeoutPeriod(),
                               replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       CancelAndRescheduleElectionTimeoutWhenProcessingHeartbeatResponseWithoutState) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_TRUE(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    log() << "processing " << request.cmdObj;
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON(true)));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    // Election timeout should remain unchanged.
    ASSERT_EQUALS(electionTimeoutWhen, replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest, SnapshotCommitting) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    OperationContextReplMock txn;
    runSingleNodeElection(getReplCoord());

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);
    OpTime time3(Timestamp(100, 3), 1);
    OpTime time4(Timestamp(100, 4), 1);
    OpTime time5(Timestamp(100, 5), 1);
    OpTime time6(Timestamp(100, 6), 1);

    getReplCoord()->onSnapshotCreate(time1, SnapshotName(1));
    getReplCoord()->onSnapshotCreate(time2, SnapshotName(2));
    getReplCoord()->onSnapshotCreate(time5, SnapshotName(3));

    // ensure current snapshot follows price is right rules (closest but not greater than)
    getReplCoord()->setMyLastOptime(time3);
    ASSERT_EQUALS(time2, getReplCoord()->getCurrentCommittedSnapshotOpTime());
    getReplCoord()->setMyLastOptime(time4);
    ASSERT_EQUALS(time2, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    // ensure current snapshot will not advance beyond existing snapshots
    getReplCoord()->setMyLastOptime(time6);
    ASSERT_EQUALS(time5, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    // ensure current snapshot updates on new snapshot if we are that far
    getReplCoord()->onSnapshotCreate(time6, SnapshotName(4));
    ASSERT_EQUALS(time6, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    // ensure dropping all snapshots should reset the current committed snapshot
    getReplCoord()->dropAllSnapshots();
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest, MoveOpTimeForward) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);
    OpTime time3(Timestamp(100, 3), 1);

    getReplCoord()->setMyLastOptime(time1);
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastOptime());
    getReplCoord()->setMyLastOptimeForward(time3);
    ASSERT_EQUALS(time3, getReplCoord()->getMyLastOptime());
    getReplCoord()->setMyLastOptimeForward(time2);
    ASSERT_EQUALS(time3, getReplCoord()->getMyLastOptime());
}

TEST_F(ReplCoordTest, LivenessForwardingForChainedMember) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))
             << "protocolVersion" << 1 << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("test1", 1234));
    OpTime optime(Timestamp(100, 2), 0);
    getReplCoord()->setMyLastOptime(optime);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime));

    // Check that we have two entries in our UpdatePosition (us and node 1).
    BSONObjBuilder cmdBuilder;
    getReplCoord()->prepareReplSetUpdatePositionCommand(&cmdBuilder);
    BSONObj cmd = cmdBuilder.done();
    std::set<long long> memberIds;
    BSONForEach(entryElement, cmd["optimes"].Obj()) {
        BSONObj entry = entryElement.Obj();
        long long memberId = entry["memberId"].Number();
        memberIds.insert(memberId);
        OpTime entryOpTime;
        bsonExtractOpTimeField(entry, "optime", &entryOpTime);
        ASSERT_EQUALS(optime, entryOpTime);
    }
    ASSERT_EQUALS(2U, memberIds.size());

    // Advance the clock far enough to cause the other node to be marked as DOWN.
    const Date_t startDate = getNet()->now();
    const Date_t endDate = startDate + Milliseconds(2000);
    getNet()->enterNetwork();
    while (getNet()->now() < endDate) {
        getNet()->runUntil(endDate);
        if (getNet()->now() < endDate) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
    }
    getNet()->exitNetwork();

    // Check there is one entry in our UpdatePosition, since we shouldn't forward for a DOWN node.
    BSONObjBuilder cmdBuilder2;
    getReplCoord()->prepareReplSetUpdatePositionCommand(&cmdBuilder2);
    BSONObj cmd2 = cmdBuilder2.done();
    std::set<long long> memberIds2;
    BSONForEach(entryElement, cmd2["optimes"].Obj()) {
        BSONObj entry = entryElement.Obj();
        long long memberId = entry["memberId"].Number();
        memberIds2.insert(memberId);
        OpTime entryOpTime;
        bsonExtractOpTimeField(entry, "optime", &entryOpTime);
        ASSERT_EQUALS(optime, entryOpTime);
    }
    ASSERT_EQUALS(1U, memberIds2.size());
}

TEST_F(ReplCoordTest, LivenessElectionTimeout) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("host"
                                << "node1:12345"
                                << "_id" << 0)
                           << BSON("host"
                                   << "node2:12345"
                                   << "_id" << 1) << BSON("host"
                                                          << "node3:12345"
                                                          << "_id" << 2) << BSON("host"
                                                                                 << "node4:12345"
                                                                                 << "_id" << 3)
                           << BSON("host"
                                   << "node5:12345"
                                   << "_id" << 4)) << "protocolVersion" << 1 << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime startingOpTime = OpTime(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastOptime(startingOpTime);

    // Receive notification that every node is up.
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(
        BSON("replSetUpdatePosition"
             << 1 << "optimes" << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 1 << "optime"
                                                           << startingOpTime.getTimestamp())
                                             << BSON("cfgver" << 2 << "memberId" << 2 << "optime"
                                                              << startingOpTime.getTimestamp())
                                             << BSON("cfgver" << 2 << "memberId" << 3 << "optime"
                                                              << startingOpTime.getTimestamp())
                                             << BSON("cfgver" << 2 << "memberId" << 4 << "optime"
                                                              << startingOpTime.getTimestamp())))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args, 0));
    // Become PRIMARY.
    simulateSuccessfulV1Election();

    // Keep two nodes alive.
    UpdatePositionArgs args1;
    ASSERT_OK(args1.initialize(
        BSON("replSetUpdatePosition"
             << 1 << "optimes" << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 1 << "optime"
                                                           << startingOpTime.getTimestamp())
                                             << BSON("cfgver" << 2 << "memberId" << 2 << "optime"
                                                              << startingOpTime.getTimestamp())))));
    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args1, 0));

    // Confirm that the node remains PRIMARY after the other two nodes are marked DOWN.
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    getNet()->runUntil(startDate + Milliseconds(1980));
    getNet()->exitNetwork();
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getReplCoord()->getMemberState().s);

    // Keep one node alive via two methods (UpdatePosition and requestHeartbeat).
    UpdatePositionArgs args2;
    ASSERT_OK(args2.initialize(
        BSON("replSetUpdatePosition"
             << 1 << "optimes" << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 1 << "optime"
                                                           << startingOpTime.getTimestamp())))));
    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args2, 0));

    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(2);
    hbArgs.setSenderId(1);
    hbArgs.setSenderHost(HostAndPort("node2", 12345));
    ReplSetHeartbeatResponse hbResp;
    ASSERT_OK(getReplCoord()->processHeartbeat(hbArgs, &hbResp));

    // Confirm that the node relinquishes PRIMARY after only one node is left UP.
    const Date_t startDate1 = getNet()->now();
    const Date_t endDate = startDate1 + Milliseconds(1980);
    getNet()->enterNetwork();
    while (getNet()->now() < endDate) {
        getNet()->runUntil(endDate);
        if (getNet()->now() < endDate) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
    }
    getNet()->exitNetwork();
    getReplCoord()->waitForStepDownFinish_forTest();
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getReplCoord()->getMemberState().s);
}

// TODO(schwerin): Unit test election id updating

}  // namespace
}  // namespace repl
}  // namespace mongo
