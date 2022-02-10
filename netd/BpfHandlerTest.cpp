/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * BpfHandlerTest.cpp - unit tests for BpfHandler.cpp
 */

#include <sys/socket.h>

#include <gtest/gtest.h>

#include "BpfHandler.h"

using namespace android::bpf;  // NOLINT(google-build-using-namespace): exempted

namespace android {
namespace net {

using base::Result;

constexpr int TEST_MAP_SIZE = 10;
constexpr int TEST_COOKIE = 1;
constexpr uid_t TEST_UID = 10086;
constexpr uid_t TEST_UID2 = 54321;
constexpr uint32_t TEST_TAG = 42;
constexpr uint32_t TEST_COUNTERSET = 1;
constexpr uint32_t TEST_PER_UID_STATS_ENTRIES_LIMIT = 3;
constexpr uint32_t TEST_TOTAL_UID_STATS_ENTRIES_LIMIT = 7;

#define ASSERT_VALID(x) ASSERT_TRUE((x).isValid())

class BpfHandlerTest : public ::testing::Test {
  protected:
    BpfHandlerTest()
        : mBh(TEST_PER_UID_STATS_ENTRIES_LIMIT, TEST_TOTAL_UID_STATS_ENTRIES_LIMIT) {}
    BpfHandler mBh;
    BpfMap<uint64_t, UidTagValue> mFakeCookieTagMap;
    BpfMap<StatsKey, StatsValue> mFakeStatsMapA;
    BpfMap<uint32_t, uint8_t> mFakeConfigurationMap;
    BpfMap<uint32_t, uint8_t> mFakeUidPermissionMap;

    void SetUp() {
        std::lock_guard guard(mBh.mMutex);
        ASSERT_EQ(0, setrlimitForTest());

        mFakeCookieTagMap.reset(createMap(BPF_MAP_TYPE_HASH, sizeof(uint64_t), sizeof(UidTagValue),
                                          TEST_MAP_SIZE, 0));
        ASSERT_VALID(mFakeCookieTagMap);

        mFakeStatsMapA.reset(createMap(BPF_MAP_TYPE_HASH, sizeof(StatsKey), sizeof(StatsValue),
                                       TEST_MAP_SIZE, 0));
        ASSERT_VALID(mFakeStatsMapA);

        mFakeConfigurationMap.reset(
                createMap(BPF_MAP_TYPE_HASH, sizeof(uint32_t), sizeof(uint8_t), 1, 0));
        ASSERT_VALID(mFakeConfigurationMap);

        mFakeUidPermissionMap.reset(
                createMap(BPF_MAP_TYPE_HASH, sizeof(uint32_t), sizeof(uint8_t), TEST_MAP_SIZE, 0));
        ASSERT_VALID(mFakeUidPermissionMap);

        mBh.mCookieTagMap.reset(dupFd(mFakeCookieTagMap.getMap()));
        ASSERT_VALID(mBh.mCookieTagMap);
        mBh.mStatsMapA.reset(dupFd(mFakeStatsMapA.getMap()));
        ASSERT_VALID(mBh.mStatsMapA);
        mBh.mConfigurationMap.reset(dupFd(mFakeConfigurationMap.getMap()));
        ASSERT_VALID(mBh.mConfigurationMap);
        // Always write to stats map A by default.
        ASSERT_RESULT_OK(mBh.mConfigurationMap.writeValue(CURRENT_STATS_MAP_CONFIGURATION_KEY,
                                                          SELECT_MAP_A, BPF_ANY));
        mBh.mUidPermissionMap.reset(dupFd(mFakeUidPermissionMap.getMap()));
        ASSERT_VALID(mBh.mUidPermissionMap);
    }

    int dupFd(const android::base::unique_fd& mapFd) {
        return fcntl(mapFd.get(), F_DUPFD_CLOEXEC, 0);
    }

    int setUpSocketAndTag(int protocol, uint64_t* cookie, uint32_t tag, uid_t uid,
                          uid_t realUid) {
        int sock = socket(protocol, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_LE(0, sock);
        *cookie = getSocketCookie(sock);
        EXPECT_NE(NONEXISTENT_COOKIE, *cookie);
        EXPECT_EQ(0, mBh.tagSocket(sock, tag, uid, realUid));
        return sock;
    }

    void expectUidTag(uint64_t cookie, uid_t uid, uint32_t tag) {
        Result<UidTagValue> tagResult = mFakeCookieTagMap.readValue(cookie);
        ASSERT_RESULT_OK(tagResult);
        EXPECT_EQ(uid, tagResult.value().uid);
        EXPECT_EQ(tag, tagResult.value().tag);
    }

    void expectNoTag(uint64_t cookie) { EXPECT_FALSE(mFakeCookieTagMap.readValue(cookie).ok()); }

    void populateFakeStats(uint64_t cookie, uint32_t uid, uint32_t tag, StatsKey* key) {
        UidTagValue cookieMapkey = {.uid = (uint32_t)uid, .tag = tag};
        EXPECT_RESULT_OK(mFakeCookieTagMap.writeValue(cookie, cookieMapkey, BPF_ANY));
        *key = {.uid = uid, .tag = tag, .counterSet = TEST_COUNTERSET, .ifaceIndex = 1};
        StatsValue statsMapValue = {.rxPackets = 1, .rxBytes = 100};
        EXPECT_RESULT_OK(mFakeStatsMapA.writeValue(*key, statsMapValue, BPF_ANY));
        key->tag = 0;
        EXPECT_RESULT_OK(mFakeStatsMapA.writeValue(*key, statsMapValue, BPF_ANY));
        // put tag information back to statsKey
        key->tag = tag;
    }

    template <class Key, class Value>
    void expectMapEmpty(BpfMap<Key, Value>& map) {
        auto isEmpty = map.isEmpty();
        EXPECT_RESULT_OK(isEmpty);
        EXPECT_TRUE(isEmpty.value());
    }

    void expectTagSocketReachLimit(uint32_t tag, uint32_t uid) {
        int sock = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_LE(0, sock);
        if (sock < 0) return;
        uint64_t sockCookie = getSocketCookie(sock);
        EXPECT_NE(NONEXISTENT_COOKIE, sockCookie);
        EXPECT_EQ(-EMFILE, mBh.tagSocket(sock, tag, uid, uid));
        expectNoTag(sockCookie);

        // Delete stats entries then tag socket success
        StatsKey key = {.uid = uid, .tag = 0, .counterSet = TEST_COUNTERSET, .ifaceIndex = 1};
        ASSERT_RESULT_OK(mFakeStatsMapA.deleteValue(key));
        EXPECT_EQ(0, mBh.tagSocket(sock, tag, uid, uid));
        expectUidTag(sockCookie, uid, tag);
    }
};

TEST_F(BpfHandlerTest, TestTagSocketV4) {
    uint64_t sockCookie;
    int v4socket = setUpSocketAndTag(AF_INET, &sockCookie, TEST_TAG, TEST_UID, TEST_UID);
    expectUidTag(sockCookie, TEST_UID, TEST_TAG);
    ASSERT_EQ(0, mBh.untagSocket(v4socket));
    expectNoTag(sockCookie);
    expectMapEmpty(mFakeCookieTagMap);
}

TEST_F(BpfHandlerTest, TestReTagSocket) {
    uint64_t sockCookie;
    int v4socket = setUpSocketAndTag(AF_INET, &sockCookie, TEST_TAG, TEST_UID, TEST_UID);
    expectUidTag(sockCookie, TEST_UID, TEST_TAG);
    ASSERT_EQ(0, mBh.tagSocket(v4socket, TEST_TAG + 1, TEST_UID + 1, TEST_UID + 1));
    expectUidTag(sockCookie, TEST_UID + 1, TEST_TAG + 1);
}

TEST_F(BpfHandlerTest, TestTagTwoSockets) {
    uint64_t sockCookie1;
    uint64_t sockCookie2;
    int v4socket1 = setUpSocketAndTag(AF_INET, &sockCookie1, TEST_TAG, TEST_UID, TEST_UID);
    setUpSocketAndTag(AF_INET, &sockCookie2, TEST_TAG, TEST_UID, TEST_UID);
    expectUidTag(sockCookie1, TEST_UID, TEST_TAG);
    expectUidTag(sockCookie2, TEST_UID, TEST_TAG);
    ASSERT_EQ(0, mBh.untagSocket(v4socket1));
    expectNoTag(sockCookie1);
    expectUidTag(sockCookie2, TEST_UID, TEST_TAG);
    ASSERT_FALSE(mFakeCookieTagMap.getNextKey(sockCookie2).ok());
}

TEST_F(BpfHandlerTest, TestTagSocketV6) {
    uint64_t sockCookie;
    int v6socket = setUpSocketAndTag(AF_INET6, &sockCookie, TEST_TAG, TEST_UID, TEST_UID);
    expectUidTag(sockCookie, TEST_UID, TEST_TAG);
    ASSERT_EQ(0, mBh.untagSocket(v6socket));
    expectNoTag(sockCookie);
    expectMapEmpty(mFakeCookieTagMap);
}

TEST_F(BpfHandlerTest, TestTagInvalidSocket) {
    int invalidSocket = -1;
    ASSERT_GT(0, mBh.tagSocket(invalidSocket, TEST_TAG, TEST_UID, TEST_UID));
    expectMapEmpty(mFakeCookieTagMap);
}

TEST_F(BpfHandlerTest, TestTagSocketWithoutPermission) {
    int sock = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_NE(-1, sock);
    ASSERT_EQ(-EPERM, mBh.tagSocket(sock, TEST_TAG, TEST_UID, TEST_UID2));
    expectMapEmpty(mFakeCookieTagMap);
}

TEST_F(BpfHandlerTest, TestTagSocketWithPermission) {
    // Grant permission to real uid. In practice, the uid permission map will be updated by
    // TrafficController::setPermissionForUids().
    uid_t realUid = TEST_UID2;
    ASSERT_RESULT_OK(mFakeUidPermissionMap.writeValue(realUid,
                     BPF_PERMISSION_UPDATE_DEVICE_STATS, BPF_ANY));

    // Tag a socket to a different uid other then realUid.
    uint64_t sockCookie;
    int v6socket = setUpSocketAndTag(AF_INET6, &sockCookie, TEST_TAG, TEST_UID, realUid);
    expectUidTag(sockCookie, TEST_UID, TEST_TAG);
    EXPECT_EQ(0, mBh.untagSocket(v6socket));
    expectNoTag(sockCookie);
    expectMapEmpty(mFakeCookieTagMap);
}

TEST_F(BpfHandlerTest, TestUntagInvalidSocket) {
    int invalidSocket = -1;
    ASSERT_GT(0, mBh.untagSocket(invalidSocket));
    int v4socket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GT(0, mBh.untagSocket(v4socket));
    expectMapEmpty(mFakeCookieTagMap);
}

TEST_F(BpfHandlerTest, TestTagSocketReachLimitFail) {
    uid_t uid = TEST_UID;
    StatsKey tagStatsMapKey[3];
    for (int i = 0; i < 3; i++) {
        uint64_t cookie = TEST_COOKIE + i;
        uint32_t tag = TEST_TAG + i;
        populateFakeStats(cookie, uid, tag, &tagStatsMapKey[i]);
    }
    expectTagSocketReachLimit(TEST_TAG, TEST_UID);
}

TEST_F(BpfHandlerTest, TestTagSocketReachTotalLimitFail) {
    StatsKey tagStatsMapKey[4];
    for (int i = 0; i < 4; i++) {
        uint64_t cookie = TEST_COOKIE + i;
        uint32_t tag = TEST_TAG + i;
        uid_t uid = TEST_UID + i;
        populateFakeStats(cookie, uid, tag, &tagStatsMapKey[i]);
    }
    expectTagSocketReachLimit(TEST_TAG, TEST_UID);
}

}  // namespace net
}  // namespace android