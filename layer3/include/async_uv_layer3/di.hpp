#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/route_group.hpp>
#include <rfl/to_view.hpp>

namespace async_uv::layer3::di {

class Error : public std::runtime_error {
public:
    explicit Error(std::string message) : std::runtime_error(std::move(message)) {}
};

namespace detail {

template <class>
struct always_false : std::false_type {};

template <class T>
inline constexpr bool always_false_v = always_false<T>::value;

template <class>
struct is_shared_ptr : std::false_type {};

template <class T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

template <class T>
inline constexpr bool is_shared_ptr_v = is_shared_ptr<std::remove_cvref_t<T>>::value;

template <class>
struct is_unique_ptr : std::false_type {};

template <class T, class Deleter>
struct is_unique_ptr<std::unique_ptr<T, Deleter>> : std::true_type {};

template <class T>
inline constexpr bool is_unique_ptr_v = is_unique_ptr<std::remove_cvref_t<T>>::value;

template <class T>
concept InjectableAggregate = std::is_class_v<T> && std::is_aggregate_v<T>;

} // namespace detail

class Container {
public:
    using ServiceKey = std::string;

    Container() = default;
    ~Container() = default;

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) = default;
    Container& operator=(Container&&) = default;

    template <class T>
    Container& singleton(std::shared_ptr<T> instance) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return singleton_impl<T>("", std::move(instance));
    }

    template <class T>
    Container& singleton(std::string key, std::shared_ptr<T> instance) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return singleton_impl<T>(std::move(key), std::move(instance));
    }

    template <class T, class Factory>
    Container& singleton(Factory&& factory) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return singleton_factory_impl<T>("", std::forward<Factory>(factory));
    }

    template <class T, class Factory>
    Container& singleton(std::string key, Factory&& factory) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return singleton_factory_impl<T>(std::move(key), std::forward<Factory>(factory));
    }

    template <class T, class Factory>
    Container& transient(Factory&& factory) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return transient_impl<T>("", std::forward<Factory>(factory));
    }

    template <class T, class Factory>
    Container& transient(std::string key, Factory&& factory) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return transient_impl<T>(std::move(key), std::forward<Factory>(factory));
    }

    template <class T>
    bool has_binding() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return has_binding_impl<T>("");
    }

    template <class T>
    bool has_binding(std::string_view key) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return has_binding_impl<T>(key);
    }

    template <class T>
    std::shared_ptr<T> resolve_shared() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return resolve_shared_impl<T>("");
    }

    template <class T>
    std::shared_ptr<T> resolve_shared(std::string_view key) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return resolve_shared_impl<T>(key);
    }

    template <class T>
    T& resolve() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return resolve_ref_impl<T>("");
    }

    template <class T>
    T& resolve(std::string_view key) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return resolve_ref_impl<T>(key);
    }

    template <class T>
    void inject(T& object) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        inject_impl(object, "");
    }

    template <class T>
    void inject(T& object, std::string_view key) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        inject_impl(object, key);
    }

private:
    enum class Lifetime : unsigned char {
        Singleton,
        Transient
    };

    struct Binding {
        Lifetime lifetime = Lifetime::Singleton;
        std::function<std::shared_ptr<void>(Container&)> factory;
        std::shared_ptr<void> cached;
    };

    using BindingTable = std::unordered_map<std::type_index, Binding>;

    struct ResolutionKey {
        std::type_index type;
        std::string key;

        bool operator==(const ResolutionKey& other) const noexcept {
            return type == other.type && key == other.key;
        }
    };

    struct ResolutionKeyHash {
        std::size_t operator()(const ResolutionKey& value) const noexcept {
            std::size_t seed = std::hash<std::type_index>{}(value.type);
            seed ^= std::hash<std::string>{}(value.key) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    class ResolutionGuard {
    public:
        ResolutionGuard(Container& container, std::type_index type, std::string key)
            : container_(container), resolving_key_{type, std::move(key)} {
            const auto [_, inserted] = container_.resolving_.insert(resolving_key_);
            if (!inserted) {
                throw Error(
                    std::string("DI cyclic dependency detected for type ")
                    + resolving_key_.type.name()
                    + " key="
                    + (resolving_key_.key.empty() ? "<default>" : resolving_key_.key));
            }
        }

        ~ResolutionGuard() {
            container_.resolving_.erase(resolving_key_);
        }

        ResolutionGuard(const ResolutionGuard&) = delete;
        ResolutionGuard& operator=(const ResolutionGuard&) = delete;

    private:
        Container& container_;
        ResolutionKey resolving_key_;
    };

    template <class T>
    static std::string type_name() {
        return typeid(T).name();
    }

    BindingTable& table_for_key(std::string_view key) {
        if (key.empty()) {
            return bindings_;
        }
        return keyed_bindings_[std::string(key)];
    }

    const BindingTable* find_table_for_key(std::string_view key) const {
        if (key.empty()) {
            return &bindings_;
        }
        auto it = keyed_bindings_.find(std::string(key));
        if (it == keyed_bindings_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    template <class T>
    bool has_binding_impl(std::string_view key) const {
        const auto* table = find_table_for_key(key);
        if (!table) {
            return false;
        }
        return table->contains(std::type_index(typeid(T)));
    }

    template <class T>
    bool has_binding_prefer_impl(std::string_view key) const {
        if (!key.empty() && has_binding_impl<T>(key)) {
            return true;
        }
        return has_binding_impl<T>("");
    }

    template <class T>
    void inject_impl(T& object, std::string_view key) {
        if constexpr (!detail::InjectableAggregate<T>) {
            return;
        } else {
            auto view = rfl::to_view(object);
            view.apply([this, key](auto field) {
                using FieldType = std::remove_cvref_t<decltype(field)>;
                using PointerType = typename FieldType::Type;
                static_assert(std::is_pointer_v<PointerType>, "rfl::to_view must expose pointer field types");

                using MemberType = std::remove_pointer_t<PointerType>;
                if constexpr (std::is_const_v<MemberType>) {
                    return;
                } else {
                    auto* slot = field.value();
                    using CleanMember = std::remove_cvref_t<MemberType>;

                    if constexpr (detail::is_shared_ptr_v<CleanMember> || std::is_pointer_v<CleanMember>) {
                        *slot = resolve_member<CleanMember>(key);
                    } else if constexpr (std::is_class_v<CleanMember>) {
                        if (has_binding_prefer_impl<CleanMember>(key) || std::is_aggregate_v<CleanMember>) {
                            *slot = resolve_member<CleanMember>(key);
                        }
                    }
                }
            });
        }
    }

    template <class T>
    Container& singleton_impl(std::string key, std::shared_ptr<T> instance) {
        if (!instance) {
            throw Error(std::string("DI singleton instance is null for type ") + type_name<T>());
        }

        Binding binding;
        binding.lifetime = Lifetime::Singleton;
        binding.cached = std::move(instance);
        binding.factory = [](Container&) -> std::shared_ptr<void> {
            throw Error("DI internal error: singleton factory should not be called when cached instance exists");
        };

        auto& table = table_for_key(key);
        table[std::type_index(typeid(T))] = std::move(binding);
        return *this;
    }

    template <class T, class Factory>
    Container& singleton_factory_impl(std::string key, Factory&& factory) {
        Binding binding;
        binding.lifetime = Lifetime::Singleton;
        binding.factory = make_factory<T>(std::forward<Factory>(factory));
        auto& table = table_for_key(key);
        table[std::type_index(typeid(T))] = std::move(binding);
        return *this;
    }

    template <class T, class Factory>
    Container& transient_impl(std::string key, Factory&& factory) {
        Binding binding;
        binding.lifetime = Lifetime::Transient;
        binding.factory = make_factory<T>(std::forward<Factory>(factory));
        auto& table = table_for_key(key);
        table[std::type_index(typeid(T))] = std::move(binding);
        return *this;
    }

    template <class T>
    Binding make_auto_binding() {
        Binding binding;
        binding.lifetime = Lifetime::Singleton;
        binding.factory = [](Container& container) -> std::shared_ptr<void> {
            return container.template construct_default<T>();
        };
        return binding;
    }

    template <class T>
    std::shared_ptr<T> resolve_shared_impl(std::string_view key) {
        auto& table = table_for_key(key);
        const std::type_index type_key(typeid(T));
        auto it = table.find(type_key);
        if (it == table.end()) {
            it = table.emplace(type_key, make_auto_binding<T>()).first;
        }

        auto& binding = it->second;
        if (binding.lifetime == Lifetime::Singleton && binding.cached) {
            return std::static_pointer_cast<T>(binding.cached);
        }

        ResolutionGuard guard(*this, type_key, std::string(key));
        std::shared_ptr<void> created = binding.factory(*this);
        if (!created) {
            throw Error(std::string("DI factory returned null for type ") + type_name<T>());
        }

        if (binding.lifetime == Lifetime::Singleton) {
            binding.cached = created;
        }
        return std::static_pointer_cast<T>(std::move(created));
    }

    template <class T>
    T& resolve_ref_impl(std::string_view key) {
        auto& table = table_for_key(key);
        const std::type_index type_key(typeid(T));
        auto it = table.find(type_key);
        if (it == table.end()) {
            it = table.emplace(type_key, make_auto_binding<T>()).first;
        }

        auto& binding = it->second;
        if (binding.lifetime == Lifetime::Transient) {
            throw Error(
                std::string("resolve<T>() cannot return reference for transient binding: ")
                + type_name<T>()
                + " key="
                + (key.empty() ? "<default>" : std::string(key))
                + ". Use resolve_shared<T>() instead.");
        }

        if (!binding.cached) {
            (void)resolve_shared_impl<T>(key);
        }
        return *std::static_pointer_cast<T>(binding.cached);
    }

    template <class T>
    std::shared_ptr<void> construct_default() {
        if constexpr (!std::is_default_constructible_v<T>) {
            throw Error(
                std::string("DI type is not registered and not default constructible: ") + type_name<T>());
        } else {
            auto instance = std::make_shared<T>();
            inject(*instance);
            return instance;
        }
    }

    template <class T, class Factory>
    static std::function<std::shared_ptr<void>(Container&)> make_factory(Factory&& factory) {
        using Fn = std::decay_t<Factory>;
        return [fn = Fn(std::forward<Factory>(factory))](Container& container) mutable -> std::shared_ptr<void> {
            if constexpr (std::is_invocable_v<Fn&, Container&>) {
                return to_erased_shared<T>(std::invoke(fn, container));
            } else if constexpr (std::is_invocable_v<Fn&>) {
                return to_erased_shared<T>(std::invoke(fn));
            } else {
                static_assert(
                    detail::always_false_v<Fn>,
                    "DI factory must be invocable as Factory() or Factory(Container&)");
            }
        };
    }

    template <class T, class Produced>
    static std::shared_ptr<void> to_erased_shared(Produced&& produced) {
        using P = std::remove_cvref_t<Produced>;

        if constexpr (std::is_convertible_v<P, std::shared_ptr<T>>) {
            return std::shared_ptr<T>(std::forward<Produced>(produced));
        } else if constexpr (detail::is_unique_ptr_v<P>) {
            return std::shared_ptr<T>(std::move(produced));
        } else if constexpr (std::is_convertible_v<P, T>) {
            return std::make_shared<T>(std::forward<Produced>(produced));
        } else {
            static_assert(
                detail::always_false_v<P>,
                "DI factory return type must be T, std::shared_ptr<T>, or std::unique_ptr<T>");
        }
    }

    template <class MemberType>
    auto resolve_member(std::string_view key) {
        using Clean = std::remove_cvref_t<MemberType>;

        if constexpr (detail::is_shared_ptr_v<Clean>) {
            using Inner = typename Clean::element_type;
            if (!key.empty() && has_binding_impl<Inner>(key)) {
                return resolve_shared_impl<Inner>(key);
            }
            return resolve_shared_impl<Inner>("");
        } else if constexpr (std::is_pointer_v<Clean>) {
            using Inner = std::remove_pointer_t<Clean>;
            if (!key.empty() && has_binding_impl<Inner>(key)) {
                return &resolve_ref_impl<Inner>(key);
            }
            return &resolve_ref_impl<Inner>("");
        } else if constexpr (std::is_copy_assignable_v<Clean>) {
            if (!key.empty() && has_binding_impl<Clean>(key)) {
                return resolve_ref_impl<Clean>(key);
            }
            return resolve_ref_impl<Clean>("");
        } else {
            throw Error(
                std::string("DI cannot assign field type, please use std::shared_ptr<T> or make it copy-assignable: ")
                + type_name<Clean>());
        }
    }

private:
    mutable std::recursive_mutex mutex_;
    BindingTable bindings_;
    std::unordered_map<ServiceKey, BindingTable> keyed_bindings_;
    std::unordered_set<ResolutionKey, ResolutionKeyHash> resolving_;
};

inline Container& global_di() {
    static Container container;
    return container;
}

inline Container& thread_local_di() {
    thread_local Container container;
    return container;
}

class RouteBinder;

template <class Controller>
class ControllerBinder;

struct ControllerMountOptions {
    std::string prefix;
    std::string key;
};

namespace detail {

template <class Controller>
std::string resolve_mount_prefix(const ControllerMountOptions& options);

template <class Controller>
void mount_controller_map(RouteBinder& binder);

} // namespace detail

template <class Controller>
using MethodHandler = Task<void>(Controller::*)(Context&);

template <class Controller>
using ConstMethodHandler = Task<void>(Controller::*)(Context&) const;

template <class Controller>
Handler handler(Container& container, MethodHandler<Controller> method) {
    return [&container, method](Context& ctx) -> Task<void> {
        auto controller = container.resolve_shared<Controller>();
        co_await (controller.get()->*method)(ctx);
    };
}

template <class Controller>
Handler handler(Container& container, ConstMethodHandler<Controller> method) {
    return [&container, method](Context& ctx) -> Task<void> {
        auto controller = container.resolve_shared<Controller>();
        co_await (controller.get()->*method)(ctx);
    };
}

template <class Controller>
Handler handler(Container& container, std::string key, MethodHandler<Controller> method) {
    return [&container, key = std::move(key), method](Context& ctx) -> Task<void> {
        std::shared_ptr<Controller> controller;
        if (!key.empty() && container.has_binding<Controller>(key)) {
            controller = container.resolve_shared<Controller>(key);
        } else {
            controller = container.resolve_shared<Controller>();
        }
        co_await (controller.get()->*method)(ctx);
    };
}

template <class Controller>
Handler handler(Container& container, std::string key, ConstMethodHandler<Controller> method) {
    return [&container, key = std::move(key), method](Context& ctx) -> Task<void> {
        std::shared_ptr<Controller> controller;
        if (!key.empty() && container.has_binding<Controller>(key)) {
            controller = container.resolve_shared<Controller>(key);
        } else {
            controller = container.resolve_shared<Controller>();
        }
        co_await (controller.get()->*method)(ctx);
    };
}

class RouteBinder {
public:
    using Builder = std::function<void(RouteBinder&)>;

    RouteBinder(App& app, Container& container) : app_(&app), container_(&container) {}
    RouteBinder(RouteGroup& group, Container& container) : group_(&group), container_(&container) {}

    RouteBinder with_key(std::string key) const {
        RouteBinder copy = *this;
        copy.controller_key_ = std::move(key);
        return copy;
    }

    RouteBinder& use(Middleware middleware) {
        if (group_) {
            group_->use(std::move(middleware));
        } else {
            app_->use(std::move(middleware));
        }
        return *this;
    }

    template <class Controller>
    RouteBinder& get(std::string_view pattern, MethodHandler<Controller> method) {
        return add_get<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& post(std::string_view pattern, MethodHandler<Controller> method) {
        return add_post<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& put(std::string_view pattern, MethodHandler<Controller> method) {
        return add_put<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& del(std::string_view pattern, MethodHandler<Controller> method) {
        return add_del<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& patch(std::string_view pattern, MethodHandler<Controller> method) {
        return add_patch<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& all(std::string_view pattern, MethodHandler<Controller> method) {
        return add_all<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& get(std::string_view pattern, ConstMethodHandler<Controller> method) {
        return add_get<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& post(std::string_view pattern, ConstMethodHandler<Controller> method) {
        return add_post<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& put(std::string_view pattern, ConstMethodHandler<Controller> method) {
        return add_put<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& del(std::string_view pattern, ConstMethodHandler<Controller> method) {
        return add_del<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& patch(std::string_view pattern, ConstMethodHandler<Controller> method) {
        return add_patch<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& all(std::string_view pattern, ConstMethodHandler<Controller> method) {
        return add_all<Controller>(pattern, method, std::nullopt);
    }

    template <class Controller>
    RouteBinder& get(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_get<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& post(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_post<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& put(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_put<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& del(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_del<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& patch(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_patch<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& all(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_all<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& get(std::string_view pattern, ConstMethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_get<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& post(std::string_view pattern, ConstMethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_post<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& put(std::string_view pattern, ConstMethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_put<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& del(std::string_view pattern, ConstMethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_del<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& patch(std::string_view pattern, ConstMethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_patch<Controller>(pattern, method, std::move(meta));
    }

    template <class Controller>
    RouteBinder& all(std::string_view pattern, ConstMethodHandler<Controller> method, Router::RouteMeta meta) {
        return add_all<Controller>(pattern, method, std::move(meta));
    }

    RouteBinder& router(std::string_view prefix, Builder builder) {
        if (group_) {
            group_->router(prefix, [this, builder = std::move(builder)](RouteGroup& child) {
                RouteBinder child_binder(child, *container_);
                child_binder.controller_key_ = controller_key_;
                builder(child_binder);
            });
        } else {
            app_->router(prefix, [this, builder = std::move(builder)](RouteGroup& child) {
                RouteBinder child_binder(child, *container_);
                child_binder.controller_key_ = controller_key_;
                builder(child_binder);
            });
        }
        return *this;
    }

    template <class Controller>
    RouteBinder& mount_controller(ControllerMountOptions options = {}) {
        RouteBinder target = options.key.empty() ? *this : with_key(std::move(options.key));
        const std::string mount_prefix = detail::resolve_mount_prefix<Controller>(options);
        if (mount_prefix.empty()) {
            detail::mount_controller_map<Controller>(target);
        } else {
            target.router(mount_prefix, [](RouteBinder& child) {
                detail::mount_controller_map<Controller>(child);
            });
        }
        return *this;
    }

private:
    template <class Controller, class Method>
    RouteBinder& add_get(
        std::string_view pattern,
        Method method,
        std::optional<Router::RouteMeta> meta) {
        Handler h = controller_key_.empty()
            ? handler<Controller>(*container_, method)
            : handler<Controller>(*container_, controller_key_, method);
        if (group_) {
            if (meta) group_->get(pattern, std::move(h), *meta);
            else group_->get(pattern, std::move(h));
        } else {
            if (meta) app_->get(pattern, std::move(h), *meta);
            else app_->get(pattern, std::move(h));
        }
        return *this;
    }

    template <class Controller, class Method>
    RouteBinder& add_post(
        std::string_view pattern,
        Method method,
        std::optional<Router::RouteMeta> meta) {
        Handler h = controller_key_.empty()
            ? handler<Controller>(*container_, method)
            : handler<Controller>(*container_, controller_key_, method);
        if (group_) {
            if (meta) group_->post(pattern, std::move(h), *meta);
            else group_->post(pattern, std::move(h));
        } else {
            if (meta) app_->post(pattern, std::move(h), *meta);
            else app_->post(pattern, std::move(h));
        }
        return *this;
    }

    template <class Controller, class Method>
    RouteBinder& add_put(
        std::string_view pattern,
        Method method,
        std::optional<Router::RouteMeta> meta) {
        Handler h = controller_key_.empty()
            ? handler<Controller>(*container_, method)
            : handler<Controller>(*container_, controller_key_, method);
        if (group_) {
            if (meta) group_->put(pattern, std::move(h), *meta);
            else group_->put(pattern, std::move(h));
        } else {
            if (meta) app_->put(pattern, std::move(h), *meta);
            else app_->put(pattern, std::move(h));
        }
        return *this;
    }

    template <class Controller, class Method>
    RouteBinder& add_del(
        std::string_view pattern,
        Method method,
        std::optional<Router::RouteMeta> meta) {
        Handler h = controller_key_.empty()
            ? handler<Controller>(*container_, method)
            : handler<Controller>(*container_, controller_key_, method);
        if (group_) {
            if (meta) group_->del(pattern, std::move(h), *meta);
            else group_->del(pattern, std::move(h));
        } else {
            if (meta) app_->del(pattern, std::move(h), *meta);
            else app_->del(pattern, std::move(h));
        }
        return *this;
    }

    template <class Controller, class Method>
    RouteBinder& add_patch(
        std::string_view pattern,
        Method method,
        std::optional<Router::RouteMeta> meta) {
        Handler h = controller_key_.empty()
            ? handler<Controller>(*container_, method)
            : handler<Controller>(*container_, controller_key_, method);
        if (group_) {
            if (meta) group_->patch(pattern, std::move(h), *meta);
            else group_->patch(pattern, std::move(h));
        } else {
            if (meta) app_->patch(pattern, std::move(h), *meta);
            else app_->patch(pattern, std::move(h));
        }
        return *this;
    }

    template <class Controller, class Method>
    RouteBinder& add_all(
        std::string_view pattern,
        Method method,
        std::optional<Router::RouteMeta> meta) {
        Handler h = controller_key_.empty()
            ? handler<Controller>(*container_, method)
            : handler<Controller>(*container_, controller_key_, method);
        if (group_) {
            if (meta) group_->all(pattern, std::move(h), *meta);
            else group_->all(pattern, std::move(h));
        } else {
            if (meta) app_->all(pattern, std::move(h), *meta);
            else app_->all(pattern, std::move(h));
        }
        return *this;
    }

private:
    App* app_ = nullptr;
    RouteGroup* group_ = nullptr;
    Container* container_ = nullptr;
    std::string controller_key_;
};

template <class Controller>
class ControllerBinder {
public:
    using Builder = std::function<void(ControllerBinder<Controller>&)>;
    using GenericBuilder = std::function<void(RouteBinder&)>;

    explicit ControllerBinder(RouteBinder binder) : binder_(std::move(binder)) {}

    ControllerBinder with_key(std::string key) const {
        return ControllerBinder(binder_.with_key(std::move(key)));
    }

    ControllerBinder& use(Middleware middleware) {
        binder_.use(std::move(middleware));
        return *this;
    }

    ControllerBinder& get(std::string_view pattern, MethodHandler<Controller> method) {
        binder_.template get<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& post(std::string_view pattern, MethodHandler<Controller> method) {
        binder_.template post<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& put(std::string_view pattern, MethodHandler<Controller> method) {
        binder_.template put<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& del(std::string_view pattern, MethodHandler<Controller> method) {
        binder_.template del<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& patch(std::string_view pattern, MethodHandler<Controller> method) {
        binder_.template patch<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& all(std::string_view pattern, MethodHandler<Controller> method) {
        binder_.template all<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& get(std::string_view pattern, ConstMethodHandler<Controller> method) {
        binder_.template get<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& post(std::string_view pattern, ConstMethodHandler<Controller> method) {
        binder_.template post<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& put(std::string_view pattern, ConstMethodHandler<Controller> method) {
        binder_.template put<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& del(std::string_view pattern, ConstMethodHandler<Controller> method) {
        binder_.template del<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& patch(std::string_view pattern, ConstMethodHandler<Controller> method) {
        binder_.template patch<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& all(std::string_view pattern, ConstMethodHandler<Controller> method) {
        binder_.template all<Controller>(pattern, method);
        return *this;
    }

    ControllerBinder& get(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        binder_.template get<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& post(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        binder_.template post<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& put(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        binder_.template put<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& del(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        binder_.template del<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& patch(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        binder_.template patch<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& all(std::string_view pattern, MethodHandler<Controller> method, Router::RouteMeta meta) {
        binder_.template all<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& get(
        std::string_view pattern,
        ConstMethodHandler<Controller> method,
        Router::RouteMeta meta) {
        binder_.template get<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& post(
        std::string_view pattern,
        ConstMethodHandler<Controller> method,
        Router::RouteMeta meta) {
        binder_.template post<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& put(
        std::string_view pattern,
        ConstMethodHandler<Controller> method,
        Router::RouteMeta meta) {
        binder_.template put<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& del(
        std::string_view pattern,
        ConstMethodHandler<Controller> method,
        Router::RouteMeta meta) {
        binder_.template del<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& patch(
        std::string_view pattern,
        ConstMethodHandler<Controller> method,
        Router::RouteMeta meta) {
        binder_.template patch<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& all(
        std::string_view pattern,
        ConstMethodHandler<Controller> method,
        Router::RouteMeta meta) {
        binder_.template all<Controller>(pattern, method, std::move(meta));
        return *this;
    }

    ControllerBinder& router(std::string_view prefix, Builder builder) {
        binder_.router(prefix, [builder = std::move(builder)](RouteBinder& child) mutable {
            ControllerBinder<Controller> child_binder(child);
            builder(child_binder);
        });
        return *this;
    }

    ControllerBinder& router(std::string_view prefix, GenericBuilder builder) {
        binder_.router(prefix, std::move(builder));
        return *this;
    }

    RouteBinder& raw() {
        return binder_;
    }

    const RouteBinder& raw() const {
        return binder_;
    }

private:
    RouteBinder binder_;
};

namespace detail {

inline std::string normalize_mount_prefix(std::string_view input) {
    if (input.empty()) {
        return "";
    }

    std::string normalized;
    normalized.reserve(input.size() + 1);

    if (input.front() != '/') {
        normalized.push_back('/');
    }

    bool previous_was_slash = !normalized.empty() && normalized.back() == '/';
    for (char ch : input) {
        if (ch == '/') {
            if (!previous_was_slash) {
                normalized.push_back(ch);
                previous_was_slash = true;
            }
        } else {
            normalized.push_back(ch);
            previous_was_slash = false;
        }
    }

    if (normalized == "/") {
        return "";
    }
    if (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

template <class Controller>
std::string resolve_mount_prefix(const ControllerMountOptions& options) {
    if (!options.prefix.empty()) {
        return normalize_mount_prefix(options.prefix);
    }

    if constexpr (requires {
                      Controller::prefix;
                  }) {
        return normalize_mount_prefix(std::string_view(Controller::prefix));
    } else {
        return "";
    }
}

template <class Controller>
void mount_controller_map(RouteBinder& binder) {
    if constexpr (requires(ControllerBinder<Controller>& cb) {
                      Controller::map(cb);
                  }) {
        ControllerBinder<Controller> controller_binder(binder);
        Controller::map(controller_binder);
    } else if constexpr (requires(RouteBinder& rb) {
                             Controller::map(rb);
                         }) {
        Controller::map(binder);
    } else {
        static_assert(
            always_false_v<Controller>,
            "Controller must define static map(ControllerBinder<Controller>&) or static map(RouteBinder&)");
    }
}

} // namespace detail

inline RouteBinder bind_routes(App& app, Container& container) {
    return RouteBinder(app, container);
}

inline RouteBinder bind_routes(RouteGroup& group, Container& container) {
    return RouteBinder(group, container);
}

template <class Controller>
RouteBinder mount_controller(App& app, Container& container, ControllerMountOptions options = {}) {
    auto binder = bind_routes(app, container);
    binder.mount_controller<Controller>(std::move(options));
    return binder;
}

template <class Controller>
RouteBinder mount_controller(RouteGroup& group, Container& container, ControllerMountOptions options = {}) {
    auto binder = bind_routes(group, container);
    binder.mount_controller<Controller>(std::move(options));
    return binder;
}

} // namespace async_uv::layer3::di
