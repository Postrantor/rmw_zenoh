// Copyright 2023 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <zenoh.h>

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "rcpputils/scope_exit.hpp"
#include "rcutils/logging_macros.h"

#include "rmw/error_handling.h"

#include "attachment_helpers.hpp"
#include "rmw_data_types.hpp"

///=============================================================================
static size_t hash_gid(const uint8_t * gid)
{
  std::stringstream hash_str;
  hash_str << std::hex;
  size_t i = 0;
  for (; i < (RMW_GID_STORAGE_SIZE - 1); i++) {
    hash_str << static_cast<int>(gid[i]);
  }
  return std::hash<std::string>{}(hash_str.str());
}

///=============================================================================
static size_t hash_gid(const rmw_request_id_t & request_id)
{
  return hash_gid(request_id.writer_guid);
}

///=============================================================================
size_t rmw_context_impl_s::get_next_entity_id()
{
  return next_entity_id_++;
}

namespace rmw_zenoh_cpp
{
///=============================================================================
saved_msg_data::saved_msg_data(
  zc_owned_payload_t p,
  uint64_t recv_ts,
  const uint8_t pub_gid[RMW_GID_STORAGE_SIZE],
  int64_t seqnum,
  int64_t source_ts)
: payload(p), recv_timestamp(recv_ts), sequence_number(seqnum), source_timestamp(source_ts)
{
  memcpy(publisher_gid, pub_gid, RMW_GID_STORAGE_SIZE);
}

///=============================================================================
saved_msg_data::~saved_msg_data()
{
  z_drop(z_move(payload));
}

///=============================================================================
size_t rmw_publisher_data_t::get_next_sequence_number()
{
  std::lock_guard<std::mutex> lock(sequence_number_mutex_);
  return sequence_number_++;
}

///=============================================================================
void rmw_subscription_data_t::attach_condition(std::condition_variable * condition_variable)
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  condition_ = condition_variable;
}

///=============================================================================
void rmw_subscription_data_t::notify()
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  if (condition_ != nullptr) {
    condition_->notify_one();
  }
}

///=============================================================================
void rmw_subscription_data_t::detach_condition()
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  condition_ = nullptr;
}

///=============================================================================
bool rmw_subscription_data_t::message_queue_is_empty() const
{
  std::lock_guard<std::mutex> lock(message_queue_mutex_);
  return message_queue_.empty();
}

///=============================================================================
std::unique_ptr<saved_msg_data> rmw_subscription_data_t::pop_next_message()
{
  std::lock_guard<std::mutex> lock(message_queue_mutex_);

  if (message_queue_.empty()) {
    // This tells rcl that the check for a new message was done, but no messages have come in yet.
    return nullptr;
  }

  std::unique_ptr<rmw_zenoh_cpp::saved_msg_data> msg_data = std::move(message_queue_.front());
  message_queue_.pop_front();

  return msg_data;
}

///=============================================================================
void rmw_subscription_data_t::add_new_message(
  std::unique_ptr<saved_msg_data> msg, const std::string & topic_name)
{
  std::lock_guard<std::mutex> lock(message_queue_mutex_);

  if (message_queue_.size() >= adapted_qos_profile.depth) {
    // Log warning if message is discarded due to hitting the queue depth
    RCUTILS_LOG_DEBUG_NAMED(
      "rmw_zenoh_cpp",
      "Message queue depth of %ld reached, discarding oldest message "
      "for subscription for %s",
      adapted_qos_profile.depth,
      topic_name.c_str());

    // If the adapted_qos_profile.depth is 0, the std::move command below will result
    // in UB and the z_drop will segfault. We explicitly set the depth to a minimum of 1
    // in rmw_create_subscription() but to be safe, we only attempt to discard from the
    // queue if it is non-empty.
    if (!message_queue_.empty()) {
      std::unique_ptr<saved_msg_data> old = std::move(message_queue_.front());
      message_queue_.pop_front();
    }
  }

  // Check for messages lost if the new sequence number is not monotonically increasing.
  const size_t gid_hash = hash_gid(msg->publisher_gid);
  auto last_known_pub_it = last_known_published_msg_.find(gid_hash);
  if (last_known_pub_it != last_known_published_msg_.end()) {
    const int64_t seq_increment = std::abs(msg->sequence_number - last_known_pub_it->second);
    if (seq_increment > 1) {
      const size_t num_msg_lost = seq_increment - 1;
      total_messages_lost_ += num_msg_lost;
      auto event_status = std::make_unique<rmw_zenoh_event_status_t>();
      event_status->total_count_change = num_msg_lost;
      event_status->total_count = total_messages_lost_;
      events_mgr.add_new_event(
        ZENOH_EVENT_MESSAGE_LOST,
        std::move(event_status));
    }
  }
  // Always update the last known sequence number for the publisher
  last_known_published_msg_[gid_hash] = msg->sequence_number;

  message_queue_.emplace_back(std::move(msg));

  // Since we added new data, trigger user callback and guard condition if they are available
  data_callback_mgr.trigger_callback();
  notify();
}

///=============================================================================
bool rmw_service_data_t::query_queue_is_empty() const
{
  std::lock_guard<std::mutex> lock(query_queue_mutex_);
  return query_queue_.empty();
}

///=============================================================================
void rmw_service_data_t::attach_condition(std::condition_variable * condition_variable)
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  condition_ = condition_variable;
}

///=============================================================================
void rmw_service_data_t::detach_condition()
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  condition_ = nullptr;
}

///=============================================================================
std::unique_ptr<ZenohQuery> rmw_service_data_t::pop_next_query()
{
  std::lock_guard<std::mutex> lock(query_queue_mutex_);
  if (query_queue_.empty()) {
    return nullptr;
  }

  std::unique_ptr<ZenohQuery> query = std::move(query_queue_.front());
  query_queue_.pop_front();

  return query;
}

///=============================================================================
void rmw_service_data_t::notify()
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  if (condition_ != nullptr) {
    condition_->notify_one();
  }
}

///=============================================================================
void rmw_service_data_t::add_new_query(std::unique_ptr<ZenohQuery> query)
{
  std::lock_guard<std::mutex> lock(query_queue_mutex_);
  if (query_queue_.size() >= adapted_qos_profile.depth) {
    // Log warning if message is discarded due to hitting the queue depth
    z_owned_str_t keystr = z_keyexpr_to_string(z_loan(this->keyexpr));
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Query queue depth of %ld reached, discarding oldest Query "
      "for service for %s",
      adapted_qos_profile.depth,
      z_loan(keystr));
    z_drop(z_move(keystr));
    query_queue_.pop_front();
  }
  query_queue_.emplace_back(std::move(query));

  // Since we added new data, trigger user callback and guard condition if they are available
  data_callback_mgr.trigger_callback();
  notify();
}

///=============================================================================
bool rmw_service_data_t::add_to_query_map(
  const rmw_request_id_t & request_id, std::unique_ptr<ZenohQuery> query)
{
  size_t hash = hash_gid(request_id);

  std::lock_guard<std::mutex> lock(sequence_to_query_map_mutex_);

  std::unordered_map<size_t, SequenceToQuery>::iterator it = sequence_to_query_map_.find(hash);

  if (it == sequence_to_query_map_.end()) {
    SequenceToQuery stq;

    sequence_to_query_map_.insert(std::make_pair(hash, std::move(stq)));

    it = sequence_to_query_map_.find(hash);
  } else {
    // Client already in the map

    if (it->second.find(request_id.sequence_number) != it->second.end()) {
      return false;
    }
  }

  it->second.insert(std::make_pair(request_id.sequence_number, std::move(query)));

  return true;
}

///=============================================================================
std::unique_ptr<ZenohQuery> rmw_service_data_t::take_from_query_map(
  const rmw_request_id_t & request_id)
{
  size_t hash = hash_gid(request_id);

  std::lock_guard<std::mutex> lock(sequence_to_query_map_mutex_);

  std::unordered_map<size_t, SequenceToQuery>::iterator it = sequence_to_query_map_.find(hash);

  if (it == sequence_to_query_map_.end()) {
    return nullptr;
  }

  SequenceToQuery::iterator query_it = it->second.find(request_id.sequence_number);

  if (query_it == it->second.end()) {
    return nullptr;
  }

  std::unique_ptr<ZenohQuery> query = std::move(query_it->second);
  it->second.erase(query_it);

  if (sequence_to_query_map_[hash].size() == 0) {
    sequence_to_query_map_.erase(hash);
  }

  return query;
}

///=============================================================================
void rmw_client_data_t::notify()
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  if (condition_ != nullptr) {
    condition_->notify_one();
  }
}

///=============================================================================
void rmw_client_data_t::add_new_reply(std::unique_ptr<ZenohReply> reply)
{
  std::lock_guard<std::mutex> lock(reply_queue_mutex_);
  if (reply_queue_.size() >= adapted_qos_profile.depth) {
    // Log warning if message is discarded due to hitting the queue depth
    z_owned_str_t keystr = z_keyexpr_to_string(z_loan(this->keyexpr));
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Reply queue depth of %ld reached, discarding oldest reply "
      "for client for %s",
      adapted_qos_profile.depth,
      z_loan(keystr));
    z_drop(z_move(keystr));
    reply_queue_.pop_front();
  }
  reply_queue_.emplace_back(std::move(reply));

  // Since we added new data, trigger user callback and guard condition if they are available
  data_callback_mgr.trigger_callback();
  notify();
}

///=============================================================================
bool rmw_client_data_t::reply_queue_is_empty() const
{
  std::lock_guard<std::mutex> lock(reply_queue_mutex_);

  return reply_queue_.empty();
}

///=============================================================================
void rmw_client_data_t::attach_condition(std::condition_variable * condition_variable)
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  condition_ = condition_variable;
}

///=============================================================================
void rmw_client_data_t::detach_condition()
{
  std::lock_guard<std::mutex> lock(condition_mutex_);
  condition_ = nullptr;
}

///=============================================================================
std::unique_ptr<ZenohReply> rmw_client_data_t::pop_next_reply()
{
  std::lock_guard<std::mutex> lock(reply_queue_mutex_);

  if (reply_queue_.empty()) {
    return nullptr;
  }

  std::unique_ptr<ZenohReply> latest_reply = std::move(reply_queue_.front());
  reply_queue_.pop_front();

  return latest_reply;
}

//==============================================================================
void sub_data_handler(
  const z_sample_t * sample,
  void * data)
{
  z_owned_str_t keystr = z_keyexpr_to_string(sample->keyexpr);
  auto drop_keystr = rcpputils::make_scope_exit(
    [&keystr]() {
      z_drop(z_move(keystr));
    });

  auto sub_data = static_cast<rmw_subscription_data_t *>(data);
  if (sub_data == nullptr) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unable to obtain rmw_subscription_data_t from data for "
      "subscription for %s",
      z_loan(keystr)
    );
    return;
  }

  uint8_t pub_gid[RMW_GID_STORAGE_SIZE];
  if (!get_gid_from_attachment(&sample->attachment, pub_gid)) {
    // We failed to get the GID from the attachment.  While this isn't fatal,
    // it is unusual and so we should report it.
    memset(pub_gid, 0, RMW_GID_STORAGE_SIZE);
    RCUTILS_LOG_ERROR_NAMED("rmw_zenoh_cpp", "Unable to obtain publisher GID from the attachment.");
  }

  int64_t sequence_number = get_int64_from_attachment(&sample->attachment, "sequence_number");
  if (sequence_number < 0) {
    // We failed to get the sequence number from the attachment.  While this
    // isn't fatal, it is unusual and so we should report it.
    sequence_number = 0;
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp", "Unable to obtain sequence number from the attachment.");
  }

  int64_t source_timestamp = get_int64_from_attachment(&sample->attachment, "source_timestamp");
  if (source_timestamp < 0) {
    // We failed to get the source timestamp from the attachment.  While this
    // isn't fatal, it is unusual and so we should report it.
    source_timestamp = 0;
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp", "Unable to obtain sequence number from the attachment.");
  }

  sub_data->add_new_message(
    std::make_unique<saved_msg_data>(
      zc_sample_payload_rcinc(sample),
      sample->timestamp.time, pub_gid, sequence_number, source_timestamp), z_loan(keystr));
}

///=============================================================================
ZenohQuery::ZenohQuery(const z_query_t * query)
{
  query_ = z_query_clone(query);
}

///=============================================================================
ZenohQuery::~ZenohQuery()
{
  z_drop(z_move(query_));
}

///=============================================================================
const z_query_t ZenohQuery::get_query() const
{
  return z_query_loan(&query_);
}

//==============================================================================
void service_data_handler(const z_query_t * query, void * data)
{
  z_owned_str_t keystr = z_keyexpr_to_string(z_query_keyexpr(query));
  auto drop_keystr = rcpputils::make_scope_exit(
    [&keystr]() {
      z_drop(z_move(keystr));
    });

  rmw_service_data_t * service_data =
    static_cast<rmw_service_data_t *>(data);
  if (service_data == nullptr) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unable to obtain rmw_service_data_t from data for "
      "service for %s",
      z_loan(keystr)
    );
    return;
  }

  service_data->add_new_query(std::make_unique<ZenohQuery>(query));
}

///=============================================================================
ZenohReply::ZenohReply(const z_owned_reply_t * reply)
{
  reply_ = *reply;
}

///=============================================================================
ZenohReply::~ZenohReply()
{
  z_reply_drop(z_move(reply_));
}

///=============================================================================
std::optional<z_sample_t> ZenohReply::get_sample() const
{
  if (z_reply_is_ok(&reply_)) {
    return z_reply_ok(&reply_);
  }

  return std::nullopt;
}

///=============================================================================
size_t rmw_client_data_t::get_next_sequence_number()
{
  std::lock_guard<std::mutex> lock(sequence_number_mutex_);
  return sequence_number_++;
}

//==============================================================================
void client_data_handler(z_owned_reply_t * reply, void * data)
{
  auto client_data = static_cast<rmw_client_data_t *>(data);
  if (client_data == nullptr) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unable to obtain client_data_t "
    );
    return;
  }
  if (!z_reply_check(reply)) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "z_reply_check returned False"
    );
    return;
  }
  if (!z_reply_is_ok(reply)) {
    z_owned_str_t keystr = z_keyexpr_to_string(z_loan(client_data->keyexpr));
    z_value_t err = z_reply_err(reply);
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "z_reply_is_ok returned False for keyexpr %s. Reason: %.*s",
      z_loan(keystr),
      (int)err.payload.len,
      err.payload.start);
    z_drop(z_move(keystr));

    return;
  }

  client_data->add_new_reply(std::make_unique<ZenohReply>(reply));
  // Since we took ownership of the reply, null it out here
  *reply = z_reply_null();
}
}  // namespace rmw_zenoh_cpp
