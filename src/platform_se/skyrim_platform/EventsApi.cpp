#include "EventsApi.h"

#include "EventSinks.h"
#include "InvalidArgumentException.h"
#include "MyUpdateTask.h"
#include "NativeObject.h"
#include "NativeValueCasts.h"
#include "NullPointerException.h"
#include "ThreadPoolWrapper.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include <RE\ConsoleLog.h>

extern ThreadPoolWrapper g_pool;
extern TaskQueue g_taskQueue;

namespace {
enum class PatternType
{
  Exact,
  StartsWith,
  EndsWith
};

class Pattern
{
public:
  static Pattern Parse(const std::string& str)
  {
    auto count = std::count(str.begin(), str.end(), '*');
    if (count == 0) {
      return { PatternType::Exact, str };
    }
    if (count > 1) {
      throw std::runtime_error(
        "Patterns can contain only one '*' at the beginning/end of string");
    }

    auto pos = str.find('*');
    if (pos == 0) {
      return { PatternType::EndsWith,
               std::string(str.begin() + 1, str.end()) };
    }
    if (pos == str.size() - 1) {
      return { PatternType::StartsWith,
               std::string(str.begin(), str.end() - 1) };
    }
    throw std::runtime_error(
      "In patterns '*' must be at the beginning/end of string");
  }

  PatternType type;
  std::string str;
};

class Handler
{
public:
  Handler() = default;

  Handler(const JsValue& handler_, std::optional<double> minSelfId_,
          std::optional<double> maxSelfId_, std::optional<Pattern> pattern_)
    : enter(handler_.GetProperty("enter"))
    , leave(handler_.GetProperty("leave"))
    , minSelfId(minSelfId_)
    , maxSelfId(maxSelfId_)
    , pattern(pattern_)
  {
  }

  bool Matches(uint32_t selfId, const std::string& eventName)
  {
    if (minSelfId.has_value() && selfId < minSelfId.value()) {
      return false;
    }
    if (maxSelfId.has_value() && selfId > maxSelfId.value()) {
      return false;
    }
    if (pattern.has_value()) {
      switch (pattern->type) {
        case PatternType::Exact:
          return eventName == pattern->str;
        case PatternType::StartsWith:
          return eventName.size() >= pattern->str.size() &&
            !memcmp(eventName.data(), pattern->str.data(),
                    pattern->str.size());
        case PatternType::EndsWith:
          return eventName.size() >= pattern->str.size() &&
            !memcmp(eventName.data() +
                      (eventName.size() - pattern->str.size()),
                    pattern->str.data(), pattern->str.size());
      }
    }
    return true;
  }

  // PerThread structure is unique for each thread
  struct PerThread
  {
    JsValue storage, context;
    bool matchesCondition = false;
  };
  std::unordered_map<DWORD, PerThread> perThread;

  // Shared between threads
  const JsValue enter, leave;
  const std::optional<Pattern> pattern;
  const std::optional<double> minSelfId;
  const std::optional<double> maxSelfId;
};

class Hook
{
public:
  Hook(std::string hookName_, std::string eventNameVariableName_,
       std::optional<std::string> succeededVariableName_)
    : hookName(hookName_)
    , eventNameVariableName(eventNameVariableName_)
    , succeededVariableName(succeededVariableName_)
  {
  }

  // Chakra thread only
  void AddHandler(const Handler& handler) { handlers.push_back(handler); }

  // Thread-safe, but it isn't too useful actually
  std::string GetName() const { return hookName; }

  // Hooks are set on game functions that are being called from multiple
  // threads. So Enter/Leave methods are thread-safe, but all private methods
  // are for Chakra thread only

  void Enter(uint32_t selfId, std::string& eventName)
  {
    DWORD owningThread = GetCurrentThreadId();

    if (hookName == "sendPapyrusEvent") {
      // If there are no handlers, do not do g_taskQueue
      bool anyMatch = false;
      for (auto& h : handlers) {
        if (h.Matches(selfId, eventName)) {
          anyMatch = true;
          break;
        }
      }
      if (!anyMatch) {
        return;
      }

      return g_taskQueue.AddTask([=] {
        std::string s = eventName;
        HandleEnter(owningThread, selfId, s);
      });
    }

    auto f = [&](int) {
      try {
        if (inProgressThreads.count(owningThread))
          throw std::runtime_error("'" + hookName + "' is already processing");
        inProgressThreads.insert(owningThread);
        HandleEnter(owningThread, selfId, eventName);
      } catch (std::exception& e) {
        auto err = std::string(e.what()) + " (while performing enter on '" +
          hookName + "')";
        g_taskQueue.AddTask([err] { throw std::runtime_error(err); });
      }
    };
    g_pool.Push(f).wait();
  }

  void Leave(bool succeeded)
  {
    DWORD owningThread = GetCurrentThreadId();

    if (hookName == "sendPapyrusEvent") {
      return;
    }

    auto f = [&](int) {
      try {
        if (!inProgressThreads.count(owningThread))
          throw std::runtime_error("'" + hookName + "' is not processing");
        inProgressThreads.erase(owningThread);
        HandleLeave(owningThread, succeeded);
      } catch (std::exception& e) {
        std::string what = e.what();
        g_taskQueue.AddTask([what] {
          throw std::runtime_error(what + " (in SendAnimationEventLeave)");
        });
      }
    };
    g_pool.Push(f).wait();
  }

private:
  void HandleEnter(DWORD owningThread, uint32_t selfId, std::string& eventName)
  {
    for (auto& h : handlers) {
      auto& perThread = h.perThread[owningThread];
      perThread.matchesCondition = h.Matches(selfId, eventName);
      if (!perThread.matchesCondition) {
        continue;
      }

      PrepareContext(perThread);
      ClearContextStorage(perThread);

      perThread.context.SetProperty("selfId", static_cast<double>(selfId));
      perThread.context.SetProperty(eventNameVariableName, eventName);
      h.enter.Call({ JsValue::Undefined(), perThread.context });

      eventName = static_cast<std::string>(
        perThread.context.GetProperty(eventNameVariableName));
    }
  }

  void PrepareContext(Handler::PerThread& h)
  {
    if (h.context.GetType() != JsValue::Type::Object) {
      h.context = JsValue::Object();
    }

    thread_local auto g_standardMap =
      JsValue::GlobalObject().GetProperty("Map");
    if (h.storage.GetType() != JsValue::Type::Object) {
      h.storage = g_standardMap.Constructor({ g_standardMap });
      h.context.SetProperty("storage", h.storage);
    }
  }

  void ClearContextStorage(Handler::PerThread& h)
  {
    thread_local auto g_standardMap =
      JsValue::GlobalObject().GetProperty("Map");
    thread_local auto g_clear =
      g_standardMap.GetProperty("prototype").GetProperty("clear");
    g_clear.Call({ h.storage });
  }

  void HandleLeave(DWORD owningThread, bool succeeded)
  {
    for (auto& h : handlers) {
      auto& perThread = h.perThread.at(owningThread);
      if (!perThread.matchesCondition) {
        continue;
      }

      PrepareContext(perThread);

      if (succeededVariableName.has_value()) {
        perThread.context.SetProperty(succeededVariableName.value(),
                                      JsValue::Bool(succeeded));
      }
      h.leave.Call({ JsValue::Undefined(), perThread.context });

      h.perThread.erase(owningThread);
    }
  }

  const std::string hookName;
  const std::string eventNameVariableName;
  const std::optional<std::string> succeededVariableName;
  std::set<DWORD> inProgressThreads;
  std::vector<Handler> handlers;
};
}

struct EventsGlobalState
{
  EventsGlobalState()
  {
    sendAnimationEvent.reset(
      new Hook("sendAnimationEvent", "animEventName", "animationSucceeded"));
    sendPapyrusEvent.reset(
      new Hook("sendPapyrusEvent", "papyrusEventName", std::nullopt));
  }

  using Callbacks = std::map<std::string, std::vector<JsValue>>;
  Callbacks callbacks;
  Callbacks callbacksOnce;
  std::shared_ptr<Hook> sendAnimationEvent;
  std::shared_ptr<Hook> sendPapyrusEvent;
} g;

namespace {
void CallCalbacks(const char* eventName, const std::vector<JsValue>& arguments,
                  bool isOnce = false)
{
  EventsGlobalState::Callbacks callbacks =
    isOnce ? g.callbacksOnce : g.callbacks;

  if (isOnce)
    g.callbacksOnce[eventName].clear();

  for (auto& f : callbacks[eventName]) {
    f.Call(arguments);
  }
}
}

void EventsApi::SendEvent(const char* eventName,
                          const std::vector<JsValue>& arguments)
{
  CallCalbacks(eventName, arguments);
  CallCalbacks(eventName, arguments, true);
}

void EventsApi::Clear()
{
  g = {};
}

void EventsApi::SendAnimationEventEnter(uint32_t selfId,
                                        std::string& animEventName) noexcept
{
  g.sendAnimationEvent->Enter(selfId, animEventName);

  /*DWORD owningThread = GetCurrentThreadId();
  auto f = [&](int) {
    try {
      if (g.sendAnimationEvent.inProgressThreads.count(owningThread))
        throw std::runtime_error("'sendAnimationEvent' is already processing");

      // This should always be done before calling throwing functions
      g.sendAnimationEvent.inProgressThreads.insert(owningThread);

      for (auto& h : g.sendAnimationEvent.handlers) {
        auto& perThread = h.perThread[owningThread];
        PrepareContext(perThread, ClearStorage::Yes);

        perThread.context.SetProperty("selfId", (double)selfId);
        perThread.context.SetProperty("animEventName", animEventName);

        h.enter.Call({ JsValue::Undefined(), perThread.context });

        animEventName =
          (std::string)perThread.context.GetProperty("animEventName");
      }
    } catch (std::exception& e) {
      std::string what = e.what();
      g_taskQueue.AddTask([what] {
        throw std::runtime_error(what + " (in SendAnimationEventEnter)");
      });
    }
  };
  g_pool.Push(f).wait();*/
}

void EventsApi::SendAnimationEventLeave(bool animationSucceeded) noexcept
{
  g.sendAnimationEvent->Leave(animationSucceeded);
  /*DWORD owningThread = GetCurrentThreadId();
  auto f = [&](int) {
    try {
      if (!g.sendAnimationEvent.inProgressThreads.count(owningThread))
        throw std::runtime_error("'sendAnimationEvent' is not processing");
      g.sendAnimationEvent.inProgressThreads.erase(owningThread);

      for (auto& h : g.sendAnimationEvent.handlers) {
        auto& perThread = h.perThread.at(owningThread);
        PrepareContext(perThread, ClearStorage::No);

        perThread.context.SetProperty("animationSucceeded",
                                      JsValue::Bool(animationSucceeded));
        h.leave.Call({ JsValue::Undefined(), perThread.context });

        h.perThread.erase(owningThread);
      }
    } catch (std::exception& e) {
      std::string what = e.what();
      g_taskQueue.AddTask([what] {
        throw std::runtime_error(what + " (in SendAnimationEventLeave)");
      });
    }
  };
  g_pool.Push(f).wait();*/
}

void EventsApi::SendPapyrusEventEnter(uint32_t selfId,
                                      std::string& papyrusEventName) noexcept
{
  g.sendPapyrusEvent->Enter(selfId, papyrusEventName);
  /*DWORD owningThread = GetCurrentThreadId();
  auto f = [&](int) {
    try {
      if (!g.sendPapyrusEvent.inProgressThreads.count(owningThread))
        throw std::runtime_error("'sendPapyrusEvent' is already processing");
      g.sendPapyrusEvent.inProgressThreads.insert(owningThread);

    } catch (std::exception& e) {
      std::string what = e.what();
      g_taskQueue.AddTask([what] {
        throw std::runtime_error(what + " (in SendPapyrusEventEnter)");
      });
    }
  };
  g_pool.Push(f).wait();*/
}

void EventsApi::SendPapyrusEventLeave() noexcept
{
  g.sendPapyrusEvent->Leave(true);
}

namespace {
JsValue CreateHookApi(std::shared_ptr<Hook> hookInfo)
{
  auto hook = JsValue::Object();
  hook.SetProperty(
    "add", JsValue::Function([hookInfo](const JsFunctionArguments& args) {
      auto handlerObj = args[1];

      std::optional<double> minSelfId;
      if (args[2].GetType() == JsValue::Type::Number) {
        minSelfId = static_cast<double>(args[2]);
      }

      std::optional<double> maxSelfId;
      if (args[3].GetType() == JsValue::Type::Number) {
        maxSelfId = static_cast<double>(args[3]);
      }

      std::optional<Pattern> pattern;
      if (args[4].GetType() == JsValue::Type::String) {
        pattern = Pattern::Parse(static_cast<std::string>(args[4]));
      }

      Handler handler(handlerObj, minSelfId, maxSelfId, pattern);
      hookInfo->AddHandler(handler);

      return JsValue::Undefined();
    }));
  return hook;
}
}

JsValue EventsApi::GetHooks()
{
  auto res = JsValue::Object();
  for (auto& hook : { g.sendAnimationEvent, g.sendPapyrusEvent }) {
    res.SetProperty(hook->GetName(), CreateHookApi(hook));
  }
  return res;
}

namespace {
JsValue AddCallback(const JsFunctionArguments& args, bool isOnce = false)
{
  static EventSinks g_sinks;

  auto eventName = args[1].ToString();
  auto callback = args[2];

  std::set<std::string> events = { "tick",
                                   "update",
                                   "effectStart",
                                   "effectFinish",
                                   "magicEffectApply",
                                   "equip",
                                   "unequip",
                                   "hit",
                                   "containerChanged",
                                   "deathStart",
                                   "deathEnd",
                                   "loadGame",
                                   "combatState",
                                   "reset",
                                   "scriptInit",
                                   "trackedStats",
                                   "uniqueIdChange",
                                   "switchRaceComplete",
                                   "cellFullyLoaded",
                                   "grabRelease",
                                   "lockChanged",
                                   "moveAttachDetach",
                                   "objectLoaded",
                                   "waitStop",
                                   "activate" };

  if (events.count(eventName) == 0)
    throw InvalidArgumentException("eventName", eventName);

  isOnce ? g.callbacksOnce[eventName].push_back(callback)
         : g.callbacks[eventName].push_back(callback);
  return JsValue::Undefined();
}
}

JsValue EventsApi::On(const JsFunctionArguments& args)
{
  return AddCallback(args);
}

JsValue EventsApi::Once(const JsFunctionArguments& args)
{
  return AddCallback(args, true);
}
