// Stand-in for ESPHome's automation.h, enough for lvgl_bench's RunAction.
#pragma once
#include <functional>
namespace esphome {
template<typename... Ts> class Action {
 public:
  virtual ~Action() = default;
  virtual void play(const Ts &...x) = 0;
};
template<typename T> class Parented {
 public:
  void set_parent(T *p) { this->parent_ = p; }
 protected:
  T *parent_{nullptr};
};
}  // namespace esphome
namespace esphome {
template<typename T, typename... X> struct TemplatableStub {
  std::function<T(const X &...)> f;
  T value(const X &...x) const { return f(x...); }
};
}  // namespace esphome
// As the codegen emits it: set_duration([]() -> uint32_t { return 10000; })
#define TEMPLATABLE_VALUE(type, name) \
 protected: \
  esphome::TemplatableStub<type, Ts...> name##_; \
 public: \
  template<typename V> void set_##name(V v) { this->name##_.f = [v](const Ts &...) -> type { return v(); }; }
