#pragma once

#include "agnocast/agnocast_epoll.hpp"
#include "agnocast/agnocast_epoll_update_dispatcher.hpp"
#include "agnocast/agnocast_smart_pointer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>

// Forward declarations to avoid pulling typesupport / rmw / serialization
// headers into every translation unit that uses callback registration.
// The full definitions are only needed in agnocast_callback_info.cpp where
// rmw_serialize is invoked.
namespace rclcpp
{
class SerializedMessage;
}
struct rosidl_message_type_support_t;

namespace agnocast
{

// Capped slightly below UINT32_MAX to provide a safe margin against
// atomic wrap-around (overflow back to 0) during concurrent fetch_add calls.
constexpr uint32_t MAX_CALLBACK_INFO_ID_SAFETY_MARGIN = 1000;
constexpr uint32_t MAX_CALLBACK_INFO_ID = UINT32_MAX - MAX_CALLBACK_INFO_ID_SAFETY_MARGIN;

struct AgnocastExecutable;

// Base class for a type-erased object
class AnyObject
{
public:
  virtual ~AnyObject() = default;
  virtual const std::type_info & type() const = 0;
};

// Class for a specific message type
template <typename T>
class TypedMessagePtr : public AnyObject
{
  agnocast::ipc_shared_ptr<T> ptr_;

public:
  explicit TypedMessagePtr(agnocast::ipc_shared_ptr<T> p) : ptr_(std::move(p)) {}

  const std::type_info & type() const override { return typeid(T); }

  agnocast::ipc_shared_ptr<T> && get() && { return std::move(ptr_); }
};

// Class for a type-erased message envelope used by GenericSubscription.
// Holds the raw shared-memory address as `ipc_shared_ptr<std::byte>`. The
// `std::byte` element type (rather than `uint8_t`) makes the "opaque memory
// blob" intent explicit at the type level — `std::byte` is a scoped enum with
// no arithmetic semantics, so it cannot be misread as a 1-byte integer.
//
// TODO(generic-subscription): migrate to `ipc_shared_ptr<std::byte[]>` once the
// smart-pointer type gains an array specialization (see follow-up work).
// Array semantics would more accurately reflect that the pointee is the start
// of a serialized message of unknown length, and would also pick up `delete[]`
// on the publisher-side reset() branch automatically. For now the publisher
// branch is unreachable for subscriber-constructed instances, so the scalar
// `delete` in `reset()` is harmless.
//
// Serialization to `rclcpp::SerializedMessage` is performed inside the
// TypeErasedCallback built by `register_generic_callback`, not here — keeping
// this envelope a pure raw-byte handle leaves the door open for future
// non-serialization consumers (e.g. zero-copy bridges, custom forwarders).
class RawMessagePtr : public AnyObject
{
  agnocast::ipc_shared_ptr<std::byte> ptr_;

public:
  explicit RawMessagePtr(agnocast::ipc_shared_ptr<std::byte> p) : ptr_(std::move(p)) {}

  const std::type_info & type() const override { return typeid(RawMessagePtr); }

  agnocast::ipc_shared_ptr<std::byte> && get() && { return std::move(ptr_); }
};

// Type for type-erased callback function
using TypeErasedCallback = std::function<void(AnyObject &&)>;

struct CallbackInfo
{
  std::string topic_name;
  topic_local_id_t subscriber_id;
  bool is_transient_local;
  mqd_t mqdes;
  rclcpp::CallbackGroup::SharedPtr callback_group;
  TypeErasedCallback callback;
  std::function<std::unique_ptr<AnyObject>(
    const void *, const std::string &, const topic_local_id_t, const uint64_t)>
    message_creator;
  bool need_epoll_update = true;
};

std::vector<std::string> get_agnocast_topics_by_group(
  const rclcpp::CallbackGroup::SharedPtr & group);

// Lock ordering: when acquiring both id2_callback_info_mtx and id2_timer_info_mtx,
// always lock id2_callback_info_mtx first to avoid deadlocks.
extern std::mutex id2_callback_info_mtx;
extern std::unordered_map<uint32_t, CallbackInfo> id2_callback_info;
extern std::atomic<uint32_t> next_callback_info_id;

uint32_t allocate_callback_info_id();

template <typename T, typename Func>
TypeErasedCallback get_erased_callback(Func && callback)
{
  return [callback = std::forward<Func>(callback)](AnyObject && arg) {
    if (typeid(T) == arg.type()) {
      auto && typed_arg = static_cast<TypedMessagePtr<T> &&>(arg);
      callback(std::move(typed_arg).get());
    } else {
      RCLCPP_ERROR(
        logger, "Agnocast internal implementation error: bad allocation when callback is called");
      close(agnocast_fd);
      exit(EXIT_FAILURE);
    }
  };
}

// Build a TypeErasedCallback that, when invoked with a RawMessagePtr,
// serializes the underlying message via `rmw_serialize` (using `type_support`)
// into a freshly allocated `rclcpp::SerializedMessage`, then hands it to the
// user-supplied `callback`. The captured raw pointer is kept alive across
// rmw_serialize and released when the wrapper returns.
// On serialization failure the entry is logged and silently dropped, matching
// the existing `RCLCPP_ERROR` + recovery pattern used elsewhere in agnocast.
TypeErasedCallback get_erased_generic_callback(
  std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
  const rosidl_message_type_support_t * type_support);

template <typename MessageT, typename Func>
uint32_t register_callback(
  Func && callback, const std::string & topic_name, const topic_local_id_t subscriber_id,
  const bool is_transient_local, mqd_t mqdes, const rclcpp::CallbackGroup::SharedPtr callback_group)
{
  // NOTE: ipc_shared_ptr<MessageT> and ipc_shared_ptr<MessageT>&& make no difference in the
  // assertion expression below, but we go with ipc_shared_ptr<MessageT>&&.
  static_assert(
    std::is_invocable_v<std::decay_t<Func>, agnocast::ipc_shared_ptr<MessageT> &&> ||
      std::is_invocable_v<std::decay_t<Func>, agnocast::ipc_shared_ptr<const MessageT> &&>,
    "Callback must be callable with ipc_shared_ptr<T> or ipc_shared_ptr<const T> (const&, &&, or "
    "by-value)");

  TypeErasedCallback erased_callback = get_erased_callback<MessageT>(std::forward<Func>(callback));

  auto message_creator = [](
                           const void * ptr, const std::string & topic_name,
                           const topic_local_id_t subscriber_id, const int64_t entry_id) {
    return std::make_unique<TypedMessagePtr<MessageT>>(agnocast::ipc_shared_ptr<MessageT>(
      const_cast<MessageT *>(static_cast<const MessageT *>(ptr)), topic_name, subscriber_id,
      entry_id));
  };

  uint32_t callback_info_id = allocate_callback_info_id();

  {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
    id2_callback_info[callback_info_id] =
      CallbackInfo{topic_name,     subscriber_id,   is_transient_local, mqdes,
                   callback_group, erased_callback, message_creator};
  }

  EpollUpdateDispatcher::get_instance().request_update_all();

  return callback_info_id;
}

// Non-templated counterpart of `register_callback` for GenericSubscription.
// For each delivered entry, the supplied `callback` is invoked with a
// `std::shared_ptr<rclcpp::SerializedMessage>` produced by `rmw_serialize`
// against `type_support`. The caller (typically `GenericSubscription`) owns
// the typesupport library / handle and must keep them alive for the lifetime
// of the registered callback.
uint32_t register_generic_callback(
  std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
  const rosidl_message_type_support_t * type_support, const std::string & topic_name,
  const topic_local_id_t subscriber_id, const bool is_transient_local, mqd_t mqdes,
  const rclcpp::CallbackGroup::SharedPtr callback_group);

void receive_and_execute_message(
  uint32_t callback_info_id, pid_t my_pid, const CallbackInfo & callback_info,
  std::mutex & ready_agnocast_executables_mutex,
  std::vector<AgnocastExecutable> & ready_agnocast_executables);

void enqueue_receive_and_execute(
  uint32_t callback_info_id, pid_t my_pid, const CallbackInfo & callback_info,
  std::mutex & ready_agnocast_executables_mutex,
  std::vector<AgnocastExecutable> & ready_agnocast_executables);

class SubscriptionEventHandler : public EpollEventHandler
{
  pid_t my_pid_;
  std::mutex * ready_agnocast_executables_mutex_;
  std::vector<AgnocastExecutable> * ready_agnocast_executables_;

public:
  SubscriptionEventHandler(
    const pid_t my_pid, std::mutex * ready_agnocast_executables_mutex,
    std::vector<AgnocastExecutable> * ready_agnocast_executables)
  : my_pid_(my_pid),
    ready_agnocast_executables_mutex_(ready_agnocast_executables_mutex),
    ready_agnocast_executables_(ready_agnocast_executables)
  {
  }

  [[nodiscard]] EpollEventType get_type() const override { return EpollEventType::Subscription; }

  void prepare_epoll(int epoll_fd, const CallbackGroupValidator & validate_callback_group) override;

  void handle(EpollEventLocalID event_local_id) override;
};

}  // namespace agnocast
