/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ValidateCmd : public BasicCommand {
public:
    ValidateCmd() : BasicCommand("validate") {}

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::validate)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& output) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        const auto cri =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        auto results = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            nss,
            cri,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent,
            {} /*query*/,
            {} /*collation*/,
            boost::none /*letParameters*/,
            boost::none /*runtimeConstants*/);

        Status firstFailedShardStatus = Status::OK();
        bool isValid = true;

        BSONObjBuilder rawResBuilder(output.subobjStart("raw"));
        for (const auto& cmdResult : results) {
            const auto& shardId = cmdResult.shardId;

            const auto& swResponse = cmdResult.swResponse;
            if (!swResponse.isOK()) {
                rawResBuilder.append(shardId.toString(),
                                     BSON("error" << swResponse.getStatus().toString()));
                if (firstFailedShardStatus.isOK())
                    firstFailedShardStatus = swResponse.getStatus();
                continue;
            }

            const auto& response = swResponse.getValue();
            if (!response.isOK()) {
                rawResBuilder.append(shardId.toString(),
                                     BSON("error" << response.status.toString()));
                if (firstFailedShardStatus.isOK())
                    firstFailedShardStatus = response.status;
                continue;
            }

            rawResBuilder.append(shardId.toString(), response.data);

            const auto status = getStatusFromCommandResult(response.data);
            if (!status.isOK()) {
                if (firstFailedShardStatus.isOK())
                    firstFailedShardStatus = status;
                continue;
            }

            if (!response.data["valid"].trueValue()) {
                isValid = false;
            }
        }
        rawResBuilder.done();

        if (firstFailedShardStatus.isOK())
            output.appendBool("valid", isValid);

        uassertStatusOK(firstFailedShardStatus);
        return true;
    }

} validateCmd;

}  // namespace
}  // namespace mongo
