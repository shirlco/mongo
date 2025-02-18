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

#include "mongo/s/request_types/commit_chunk_migration_request_type.h"

#include "mongo/bson/util/bson_extract.h"

namespace mongo {
namespace {

const char kConfigSvrCommitChunkMigration[] = "_configsvrCommitChunkMigration";
const char kFromShard[] = "fromShard";
const char kToShard[] = "toShard";
const char kMigratedChunk[] = "migratedChunk";
const char kFromShardCollectionVersion[] = "fromShardCollectionVersion";
const char kValidAfter[] = "validAfter";

/**
 * Attempts to parse a (range-only!) ChunkType from "field" in "source".
 */
StatusWith<ChunkType> extractChunk(const BSONObj& source, StringData field) {
    BSONElement fieldElement;
    auto status = bsonExtractTypedField(source, field, BSONType::Object, &fieldElement);
    if (!status.isOK())
        return status;

    const auto fieldObj = fieldElement.Obj();

    auto rangeWith = ChunkRange::fromBSON(fieldObj);
    if (!rangeWith.isOK())
        return rangeWith.getStatus();

    ChunkVersion version;
    try {
        version = ChunkVersion::parse(fieldObj[ChunkType::lastmod()]);
        uassert(644490, "Version must be set", version.isSet());
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    ChunkType chunk;
    chunk.setMin(rangeWith.getValue().getMin());
    chunk.setMax(rangeWith.getValue().getMax());
    chunk.setVersion(version);
    return chunk;
}

/**
 * Attempts to parse a ShardId from "field" in "source".
 */
StatusWith<ShardId> extractShardId(const BSONObj& source, StringData field) {
    std::string stringResult;

    auto status = bsonExtractStringField(source, field, &stringResult);
    if (!status.isOK()) {
        return status;
    }

    if (stringResult.empty()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "The field '" + field.toString() + "' cannot be empty");
    }

    return ShardId(stringResult);
}

}  // namespace

StatusWith<CommitChunkMigrationRequest> CommitChunkMigrationRequest::createFromCommand(
    const NamespaceString& nss, const BSONObj& obj) {

    auto migratedChunk = extractChunk(obj, kMigratedChunk);
    if (!migratedChunk.isOK()) {
        return migratedChunk.getStatus();
    }

    CommitChunkMigrationRequest request(nss, std::move(migratedChunk.getValue()));

    {
        auto fromShard = extractShardId(obj, kFromShard);
        if (!fromShard.isOK()) {
            return fromShard.getStatus();
        }

        request._fromShard = std::move(fromShard.getValue());
    }

    {
        auto toShard = extractShardId(obj, kToShard);
        if (!toShard.isOK()) {
            return toShard.getStatus();
        }

        request._toShard = std::move(toShard.getValue());
    }

    try {
        auto fromShardVersion = ChunkVersion::parse(obj[kFromShardCollectionVersion]);
        request._collectionEpoch = fromShardVersion.epoch();
        request._collectionTimestamp = fromShardVersion.getTimestamp();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    {
        Timestamp validAfter;
        auto status = bsonExtractTimestampField(obj, kValidAfter, &validAfter);
        if (!status.isOK() && status != ErrorCodes::NoSuchKey) {
            return status;
        }

        if (status.isOK()) {
            request._validAfter = validAfter;
        } else {
            request._validAfter = boost::none;
        }
    }

    return request;
}

void CommitChunkMigrationRequest::appendAsCommand(BSONObjBuilder* builder,
                                                  const NamespaceString& nss,
                                                  const ShardId& fromShard,
                                                  const ShardId& toShard,
                                                  const ChunkType& migratedChunk,
                                                  const ChunkVersion& fromShardCollectionVersion,
                                                  const Timestamp& validAfter) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());

    builder->append(kConfigSvrCommitChunkMigration, nss.ns());
    builder->append(kFromShard, fromShard.toString());
    builder->append(kToShard, toShard.toString());
    {
        BSONObjBuilder migrateChunk(builder->subobjStart(kMigratedChunk));
        migratedChunk.getRange().append(&migrateChunk);
        migratedChunk.getVersion().serializeToBSON(ChunkType::lastmod(), &migrateChunk);
    }
    fromShardCollectionVersion.serializeToBSON(kFromShardCollectionVersion, builder);
    builder->append(kValidAfter, validAfter);
}

}  // namespace mongo
