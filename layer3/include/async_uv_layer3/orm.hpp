#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <concepts>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <async_uv_sql/sql.h>
#include <async_uv/cancel.h>
#include <async_uv/error.h>
#include <rfl/Attribute.hpp>
#include <rfl/json/read.hpp>
#include <rfl/to_view.hpp>
#include <rfl/type_name_t.hpp>

namespace async_uv::layer3::orm {

template <class T>
using List = std::vector<T>;

enum class TxPropagation {
    required,
    nested,
    requires_new,
};

enum class TxIsolation {
    default_level,
    read_uncommitted,
    read_committed,
    repeatable_read,
    serializable,
};

struct TxOptions {
    TxPropagation propagation = TxPropagation::required;
    TxIsolation isolation = TxIsolation::default_level;
    bool read_only = false;
    int timeout_ms = 0;
};

enum class CountMode {
    filtered,
    window,
};

enum class RawSqlPolicy {
    permissive,
    require_unsafe_marker,
};

struct UnsafeSql {
    std::string sql;
};

inline UnsafeSql unsafe_sql(std::string sql) {
    return UnsafeSql{.sql = std::move(sql)};
}

template <class Model>
class Mapper;

template <class Model>
class QueryChain;

template <class Model>
class UpdateChain;

template <class Model>
class Service;

template <class Model>
struct UpdateAssignment {
    std::string column;
    std::string sql_expression;
    std::vector<async_uv::sql::SqlParam> params;
};

class Error : public std::runtime_error {
public:
    explicit Error(std::string message) : std::runtime_error(std::move(message)) {}
};

using RawSqlAuditHook = std::function<void(std::string_view api,
                                           std::string_view sql,
                                           bool unsafe_marked)>;

template <class T>
struct TypeConverter {};

template <class T, class Converter>
struct As : rfl::Attribute<T> {
    using rfl::Attribute<T>::Attribute;
};

template <class T, class Converter>
struct TypeConverter<As<T, Converter>> {
    static async_uv::sql::SqlParam to_sql_param(const As<T, Converter>& value) {
        return Converter::to_sql_param(value.get());
    }

    static void from_sql_text(std::string_view text, As<T, Converter>& out) {
        T value{};
        Converter::from_sql_text(text, value);
        out = std::move(value);
    }

    static std::string_view sql_type() {
        return Converter::sql_type();
    }
};

template <class T>
struct Id : rfl::Attribute<T> {
    using rfl::Attribute<T>::Attribute;
};

template <class T>
struct AutoId : rfl::Attribute<T> {
    using rfl::Attribute<T>::Attribute;
};

template <class T>
struct Unique : rfl::Attribute<T> {
    using rfl::Attribute<T>::Attribute;
};

template <class T>
struct Indexed : rfl::Attribute<T> {
    using rfl::Attribute<T>::Attribute;
};

namespace detail {

template <class>
struct always_false : std::false_type {};

template <class T>
inline constexpr bool always_false_v = always_false<T>::value;

inline RawSqlPolicy& raw_sql_policy_ref() {
    static RawSqlPolicy policy = RawSqlPolicy::permissive;
    return policy;
}

inline RawSqlAuditHook& raw_sql_audit_hook_ref() {
    static RawSqlAuditHook hook;
    return hook;
}

inline std::mutex& raw_sql_guard_mutex() {
    static std::mutex m;
    return m;
}

inline void validate_raw_sql(
    std::string_view api,
    std::string_view sql_fragment,
    bool unsafe_marked) {
    if (sql_fragment.empty()) {
        throw Error(std::string(api) + " SQL fragment cannot be empty");
    }

    RawSqlPolicy policy = RawSqlPolicy::permissive;
    RawSqlAuditHook hook;
    {
        std::lock_guard<std::mutex> lock(raw_sql_guard_mutex());
        policy = raw_sql_policy_ref();
        hook = raw_sql_audit_hook_ref();
    }

    if (policy == RawSqlPolicy::require_unsafe_marker && !unsafe_marked) {
        throw Error(
            std::string(api)
            + " requires explicit unsafe marker; wrap SQL with orm::unsafe_sql(...)");
    }

    if (hook) {
        hook(api, sql_fragment, unsafe_marked);
    }
}

template <class T, class = void>
struct has_type_converter : std::false_type {};

template <class T>
struct has_type_converter<
    T,
    std::void_t<
        decltype(TypeConverter<T>::to_sql_param(std::declval<const T&>())),
        decltype(TypeConverter<T>::from_sql_text(std::declval<std::string_view>(), std::declval<T&>()))>>
    : std::true_type {};

template <class T>
inline constexpr bool has_type_converter_v = has_type_converter<T>::value;

template <class T, class = void>
struct has_type_converter_sql_type : std::false_type {};

template <class T>
struct has_type_converter_sql_type<T, std::void_t<decltype(TypeConverter<T>::sql_type())>> : std::true_type {};

template <class T>
inline constexpr bool has_type_converter_sql_type_v = has_type_converter_sql_type<T>::value;

template <class T>
struct is_optional : std::false_type {};

template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

template <class T>
concept AttrLike = requires(T& value) {
    typename T::Type;
    value.get();
};

template <class T>
struct FieldTraits {
    using ValueType = std::remove_cvref_t<T>;
    static constexpr bool is_id = false;
    static constexpr bool is_auto_increment = false;
    static constexpr bool is_unique = false;
    static constexpr bool is_indexed = false;
    static constexpr bool is_optional = false;
};

template <class T>
struct FieldTraits<std::optional<T>> {
    using Inner = FieldTraits<T>;
    using ValueType = typename Inner::ValueType;
    static constexpr bool is_id = Inner::is_id;
    static constexpr bool is_auto_increment = Inner::is_auto_increment;
    static constexpr bool is_unique = Inner::is_unique;
    static constexpr bool is_indexed = Inner::is_indexed;
    static constexpr bool is_optional = true;
};

template <class T>
struct FieldTraits<Id<T>> {
    using Inner = FieldTraits<T>;
    using ValueType = typename Inner::ValueType;
    static constexpr bool is_id = true;
    static constexpr bool is_auto_increment = Inner::is_auto_increment;
    static constexpr bool is_unique = Inner::is_unique;
    static constexpr bool is_indexed = Inner::is_indexed;
    static constexpr bool is_optional = Inner::is_optional;
};

template <class T>
struct FieldTraits<AutoId<T>> {
    using Inner = FieldTraits<T>;
    using ValueType = typename Inner::ValueType;
    static constexpr bool is_id = true;
    static constexpr bool is_auto_increment = true;
    static constexpr bool is_unique = Inner::is_unique;
    static constexpr bool is_indexed = Inner::is_indexed;
    static constexpr bool is_optional = Inner::is_optional;
};

template <class T>
struct FieldTraits<Unique<T>> {
    using Inner = FieldTraits<T>;
    using ValueType = typename Inner::ValueType;
    static constexpr bool is_id = Inner::is_id;
    static constexpr bool is_auto_increment = Inner::is_auto_increment;
    static constexpr bool is_unique = true;
    static constexpr bool is_indexed = Inner::is_indexed;
    static constexpr bool is_optional = Inner::is_optional;
};

template <class T>
struct FieldTraits<Indexed<T>> {
    using Inner = FieldTraits<T>;
    using ValueType = typename Inner::ValueType;
    static constexpr bool is_id = Inner::is_id;
    static constexpr bool is_auto_increment = Inner::is_auto_increment;
    static constexpr bool is_unique = Inner::is_unique;
    static constexpr bool is_indexed = true;
    static constexpr bool is_optional = Inner::is_optional;
};

template <class T>
decltype(auto) unwrap_attr(T& value) {
    if constexpr (AttrLike<T>) {
        return unwrap_attr(value.get());
    } else {
        return (value);
    }
}

template <class T>
decltype(auto) unwrap_attr(const T& value) {
    if constexpr (AttrLike<T>) {
        return unwrap_attr(value.get());
    } else {
        return (value);
    }
}

inline std::string to_snake_case(std::string name) {
    std::string out;
    out.reserve(name.size() + 8);
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        if (std::isupper(ch)) {
            if (!out.empty()) {
                const unsigned char prev = static_cast<unsigned char>(name[i - 1]);
                if (!std::isupper(prev)) {
                    out.push_back('_');
                }
            }
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return out;
}

inline std::string quote_ident(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 2);
    out.push_back('"');
    for (char ch : ident) {
        if (ch == '"') {
            out.push_back('"');
            out.push_back('"');
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

inline bool is_simple_ident(std::string_view ident) {
    if (ident.empty()) {
        return false;
    }
    const auto is_ident_char = [](char ch) {
        const unsigned char c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || ch == '_';
    };
    const unsigned char first = static_cast<unsigned char>(ident.front());
    if (!(std::isalpha(first) || ident.front() == '_')) {
        return false;
    }
    return std::all_of(ident.begin() + 1, ident.end(), is_ident_char);
}

inline bool is_simple_column_ref(std::string_view ref) {
    if (ref.empty()) {
        return false;
    }
    if (ref == "*") {
        return true;
    }

    std::size_t begin = 0;
    while (begin < ref.size()) {
        const auto end = ref.find('.', begin);
        const auto token = ref.substr(begin, end == std::string_view::npos ? ref.size() - begin : end - begin);
        if (token.empty()) {
            return false;
        }
        const bool last = end == std::string_view::npos;
        if (token == "*") {
            if (!last) {
                return false;
            }
        } else if (!is_simple_ident(token)) {
            return false;
        }

        if (last) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

inline std::string quote_column_ref(std::string_view ref) {
    if (!is_simple_column_ref(ref)) {
        return std::string(ref);
    }
    if (ref == "*") {
        return "*";
    }

    std::string out;
    std::size_t begin = 0;
    bool first = true;
    while (begin < ref.size()) {
        const auto end = ref.find('.', begin);
        const auto token = ref.substr(begin, end == std::string_view::npos ? ref.size() - begin : end - begin);
        if (!first) {
            out += ".";
        }
        if (token == "*") {
            out += "*";
        } else {
            out += quote_ident(token);
        }

        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
        first = false;
    }
    return out;
}

inline std::string qualify_and_quote_column(std::string_view default_table_ref, std::string_view column_ref) {
    if (is_simple_column_ref(column_ref) && column_ref.find('.') == std::string_view::npos
        && column_ref != "*" && !default_table_ref.empty()) {
        std::string qualified;
        qualified.reserve(default_table_ref.size() + column_ref.size() + 1);
        qualified.append(default_table_ref);
        qualified.push_back('.');
        qualified.append(column_ref);
        return quote_column_ref(qualified);
    }
    return quote_column_ref(column_ref);
}

template <class Model>
std::string resolve_table_name() {
    if constexpr (requires {
                      Model::table_name;
                  }) {
        return std::string(Model::table_name);
    } else if constexpr (requires {
                             Model::table;
                         }) {
        return std::string(Model::table);
    } else {
        std::string raw = rfl::type_name_t<Model>().str();
        const std::size_t pos = raw.rfind("::");
        if (pos != std::string::npos) {
            raw = raw.substr(pos + 2);
        }
        return to_snake_case(std::move(raw));
    }
}

template <class T>
async_uv::sql::SqlParam to_sql_param(const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, async_uv::sql::SqlParam>) {
        return value;
    } else if constexpr (has_type_converter_v<U>) {
        return TypeConverter<U>::to_sql_param(value);
    } else if constexpr (AttrLike<U>) {
        return to_sql_param(value.get());
    } else if constexpr (is_optional_v<U>) {
        if (!value.has_value()) {
            return async_uv::sql::SqlParam(nullptr);
        }
        return to_sql_param(*value);
    } else if constexpr (std::is_same_v<U, std::string>) {
        return async_uv::sql::SqlParam(value);
    } else if constexpr (std::is_same_v<U, std::string_view>) {
        return async_uv::sql::SqlParam(std::string(value));
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return async_uv::sql::SqlParam(value == nullptr ? "" : value);
    } else if constexpr (std::is_array_v<U> && std::is_same_v<std::remove_extent_t<U>, const char>) {
        return async_uv::sql::SqlParam(std::string(value));
    } else if constexpr (std::is_array_v<U> && std::is_same_v<std::remove_extent_t<U>, char>) {
        return async_uv::sql::SqlParam(std::string(value));
    } else if constexpr (std::is_same_v<U, bool>) {
        return async_uv::sql::SqlParam(value);
    } else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, bool>) {
        return async_uv::sql::SqlParam(static_cast<std::int64_t>(value));
    } else if constexpr (std::is_floating_point_v<U>) {
        return async_uv::sql::SqlParam(static_cast<double>(value));
    } else if constexpr (std::is_enum_v<U>) {
        return async_uv::sql::SqlParam(static_cast<std::int64_t>(std::to_underlying(value)));
    } else {
        static_assert(always_false_v<U>, "unsupported SQL parameter type");
    }
}

template <class T>
bool is_default_value(const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (AttrLike<U>) {
        return is_default_value(value.get());
    } else if constexpr (is_optional_v<U>) {
        return !value.has_value();
    } else if constexpr (requires {
                             U{};
                             { value == U{} } -> std::convertible_to<bool>;
                         }) {
        return value == U{};
    } else {
        return false;
    }
}

template <class T>
void assign_non_null(std::string_view text, T& out) {
    using U = std::remove_cvref_t<T>;
    if constexpr (has_type_converter_v<U>) {
        TypeConverter<U>::from_sql_text(text, out);
    } else if constexpr (std::is_same_v<U, std::string>) {
        out = std::string(text);
    } else if constexpr (std::is_same_v<U, bool>) {
        if (text == "1" || text == "true" || text == "TRUE" || text == "True") {
            out = true;
        } else if (text == "0" || text == "false" || text == "FALSE" || text == "False") {
            out = false;
        } else {
            throw Error("invalid boolean value: " + std::string(text));
        }
    } else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, bool>) {
        try {
            if constexpr (std::is_signed_v<U>) {
                const auto parsed = std::stoll(std::string(text));
                out = static_cast<U>(parsed);
            } else {
                const auto parsed = std::stoull(std::string(text));
                out = static_cast<U>(parsed);
            }
        } catch (const std::exception&) {
            throw Error("invalid integral value: " + std::string(text));
        }
    } else if constexpr (std::is_floating_point_v<U>) {
        try {
            out = static_cast<U>(std::stod(std::string(text)));
        } catch (const std::exception&) {
            throw Error("invalid floating value: " + std::string(text));
        }
    } else if constexpr (std::is_enum_v<U>) {
        using Underlying = std::underlying_type_t<U>;
        Underlying parsed{};
        assign_non_null(text, parsed);
        out = static_cast<U>(parsed);
    } else {
        const auto parsed = rfl::json::read<U>(std::string(text));
        if (!parsed) {
            throw Error("failed to parse complex type from JSON cell");
        }
        out = *parsed;
    }
}

template <class T>
void assign_from_cell(const async_uv::sql::Cell& cell, T& out) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        if (!cell.has_value()) {
            out.reset();
        } else {
            typename U::value_type value{};
            assign_non_null(*cell, value);
            out = std::move(value);
        }
    } else if constexpr (has_type_converter_v<U>) {
        if (!cell.has_value()) {
            throw Error("received NULL for non-null field");
        }
        assign_non_null(*cell, out);
    } else if constexpr (AttrLike<U>) {
        assign_from_cell(cell, out.get());
    } else {
        if (!cell.has_value()) {
            throw Error("received NULL for non-null field");
        }
        assign_non_null(*cell, out);
    }
}

template <class T>
std::string sql_type_name() {
    using U = std::remove_cvref_t<T>;
    if constexpr (has_type_converter_sql_type_v<U>) {
        return std::string(TypeConverter<U>::sql_type());
    } else if constexpr (std::is_integral_v<U> || std::is_same_v<U, bool> || std::is_enum_v<U>) {
        return "INTEGER";
    } else if constexpr (std::is_floating_point_v<U>) {
        return "REAL";
    } else {
        return "TEXT";
    }
}

} // namespace detail

inline void set_raw_sql_policy(RawSqlPolicy policy) {
    std::lock_guard<std::mutex> lock(detail::raw_sql_guard_mutex());
    detail::raw_sql_policy_ref() = policy;
}

inline RawSqlPolicy raw_sql_policy() {
    std::lock_guard<std::mutex> lock(detail::raw_sql_guard_mutex());
    return detail::raw_sql_policy_ref();
}

inline void set_raw_sql_audit_hook(RawSqlAuditHook hook) {
    std::lock_guard<std::mutex> lock(detail::raw_sql_guard_mutex());
    detail::raw_sql_audit_hook_ref() = std::move(hook);
}

inline void clear_raw_sql_audit_hook() {
    std::lock_guard<std::mutex> lock(detail::raw_sql_guard_mutex());
    detail::raw_sql_audit_hook_ref() = RawSqlAuditHook{};
}

namespace detail {

template <class Model>
struct ColumnMeta {
    std::string name;
    std::size_t offset = 0;
    bool is_id = false;
    bool is_auto_increment = false;
    bool is_unique = false;
    bool is_indexed = false;
    bool is_optional = false;
    std::string sql_type;
    std::function<async_uv::sql::SqlParam(const Model&)> read_param;
    std::function<void(Model&, const async_uv::sql::Cell&)> write_cell;
    std::function<bool(const Model&)> is_default;
};

template <class Model>
struct ModelMeta {
    std::string table;
    std::vector<ColumnMeta<Model>> columns;
    std::unordered_map<std::size_t, std::size_t> column_index_by_offset;
    std::optional<std::size_t> id_index;
    std::vector<std::size_t> unique_indexes;
};

template <class Model>
const ModelMeta<Model>& model_meta() {
    static ModelMeta<Model> meta = [] {
        static_assert(std::is_default_constructible_v<Model>, "ORM model must be default constructible");

        ModelMeta<Model> out;
        out.table = resolve_table_name<Model>();

        Model probe{};
        auto view = rfl::to_view(probe);
        view.apply([&](auto field) {
            using FieldType = std::remove_cvref_t<decltype(field)>;
            using PointerType = typename FieldType::Type;
            static_assert(std::is_pointer_v<PointerType>, "rfl::to_view must expose pointer field types");

            using MemberType = std::remove_cvref_t<std::remove_pointer_t<PointerType>>;
            using Traits = FieldTraits<MemberType>;
            using ValueType = typename Traits::ValueType;

            auto* slot = field.value();
            const auto offset = static_cast<std::size_t>(
                reinterpret_cast<const char*>(slot) - reinterpret_cast<const char*>(&probe));

            ColumnMeta<Model> col;
            col.name = std::string(field.name());
            col.offset = offset;
            col.is_id = Traits::is_id;
            col.is_auto_increment = Traits::is_auto_increment;
            col.is_unique = Traits::is_unique;
            col.is_indexed = Traits::is_indexed;
            col.is_optional = Traits::is_optional;
            col.sql_type = sql_type_name<ValueType>();

            col.read_param = [offset](const Model& model) -> async_uv::sql::SqlParam {
                const auto* member = reinterpret_cast<const MemberType*>(
                    reinterpret_cast<const char*>(&model) + offset);
                return to_sql_param(*member);
            };

            col.write_cell = [offset, col_name = col.name](Model& model, const async_uv::sql::Cell& cell) {
                auto* member = reinterpret_cast<MemberType*>(
                    reinterpret_cast<char*>(&model) + offset);
                try {
                    assign_from_cell(cell, *member);
                } catch (const std::exception& ex) {
                    throw Error("failed to map column '" + col_name + "': " + ex.what());
                }
            };

            col.is_default = [offset](const Model& model) {
                const auto* member = reinterpret_cast<const MemberType*>(
                    reinterpret_cast<const char*>(&model) + offset);
                return is_default_value(*member);
            };

            const std::size_t index = out.columns.size();
            out.column_index_by_offset[col.offset] = index;
            if (col.is_id && !out.id_index.has_value()) {
                out.id_index = index;
            }
            if (col.is_unique) {
                out.unique_indexes.push_back(index);
            }
            out.columns.push_back(std::move(col));
        });

        if (out.columns.empty()) {
            throw Error("ORM model has no fields");
        }

        return out;
    }();

    return meta;
}

template <class Model, class Member>
std::string resolve_column_name(Member member) {
    static_assert(std::is_member_object_pointer_v<Member>, "member must be a pointer to data member");

    Model probe{};
    auto* slot = std::addressof(probe.*member);
    const std::size_t offset = static_cast<std::size_t>(
        reinterpret_cast<const char*>(slot) - reinterpret_cast<const char*>(&probe));

    const auto& meta = model_meta<Model>();
    const auto it = meta.column_index_by_offset.find(offset);
    if (it == meta.column_index_by_offset.end()) {
        throw Error("failed to resolve column name from member pointer");
    }
    return meta.columns[it->second].name;
}

template <class Model, class Method>
std::string resolve_column_name_from_getter(Method method) {
    static_assert(std::is_member_function_pointer_v<Method>, "method must be a member function pointer");

    Model probe{};
    using ReturnType = std::invoke_result_t<Method, Model&>;
    static_assert(
        std::is_lvalue_reference_v<ReturnType>,
        "getter for ORM query must return lvalue reference to a model field");

    auto* slot = std::addressof((probe.*method)());
    const std::size_t offset = static_cast<std::size_t>(
        reinterpret_cast<const char*>(slot) - reinterpret_cast<const char*>(&probe));

    const auto& meta = model_meta<Model>();
    const auto it = meta.column_index_by_offset.find(offset);
    if (it == meta.column_index_by_offset.end()) {
        throw Error("failed to resolve column name from getter");
    }
    return meta.columns[it->second].name;
}

template <class T>
struct key_storage_type {
    using type = std::remove_cvref_t<T>;
};

template <>
struct key_storage_type<std::string_view> {
    using type = std::string;
};

template <>
struct key_storage_type<const char*> {
    using type = std::string;
};

template <>
struct key_storage_type<char*> {
    using type = std::string;
};

template <class T>
using key_storage_type_t = typename key_storage_type<std::remove_cvref_t<T>>::type;

template <class T>
struct remove_optional {
    using type = std::remove_cvref_t<T>;
};

template <class T>
struct remove_optional<std::optional<T>> {
    using type = std::remove_cvref_t<T>;
};

template <class T>
using remove_optional_t = typename remove_optional<std::remove_cvref_t<T>>::type;

template <class T>
using unwrap_attr_value_t = std::remove_cvref_t<decltype(unwrap_attr(std::declval<const T&>()))>;

template <class T>
using normalized_key_value_t =
    key_storage_type_t<unwrap_attr_value_t<remove_optional_t<unwrap_attr_value_t<T>>>>;

template <class Model, class Member>
using member_key_t = normalized_key_value_t<decltype(std::invoke(std::declval<Member>(), std::declval<const Model&>()))>;

template <class T, class = void>
struct is_hashable_key : std::false_type {};

template <class T>
struct is_hashable_key<T, std::void_t<decltype(std::declval<std::size_t&>() = std::hash<T>{}(std::declval<const T&>()))>>
    : std::true_type {};

template <class T>
inline constexpr bool is_hashable_key_v = is_hashable_key<T>::value;

template <class To, class From>
To cast_key(const From& value) {
    if constexpr (std::is_same_v<To, std::string>) {
        if constexpr (std::is_same_v<std::remove_cvref_t<From>, std::string>) {
            return value;
        } else if constexpr (std::is_convertible_v<From, std::string_view>) {
            return std::string(std::string_view(value));
        } else if constexpr (
            std::is_same_v<std::remove_cvref_t<From>, const char*>
            || std::is_same_v<std::remove_cvref_t<From>, char*>) {
            return std::string(value == nullptr ? "" : value);
        } else {
            return std::string(value);
        }
    } else if constexpr (std::is_same_v<To, std::remove_cvref_t<From>>) {
        return value;
    } else {
        return static_cast<To>(value);
    }
}

template <class Model, class Member>
std::optional<member_key_t<Model, Member>> extract_member_key(const Model& model, Member member) {
    auto&& raw = std::invoke(member, model);
    auto&& unwrapped = unwrap_attr(raw);
    using Unwrapped = std::remove_cvref_t<decltype(unwrapped)>;
    using Key = member_key_t<Model, Member>;

    if constexpr (is_optional_v<Unwrapped>) {
        if (!unwrapped.has_value()) {
            return std::nullopt;
        }
        auto&& inner = unwrap_attr(*unwrapped);
        return cast_key<Key>(inner);
    } else {
        auto&& inner = unwrap_attr(unwrapped);
        return cast_key<Key>(inner);
    }
}

template <class Key>
std::vector<Key> dedupe_keys(std::vector<Key> keys) {
    std::vector<Key> out;
    out.reserve(keys.size());

    std::unordered_set<Key> seen;
    seen.reserve(keys.size() * 2 + 1);

    for (auto& key : keys) {
        if (seen.insert(key).second) {
            out.push_back(std::move(key));
        }
    }

    return out;
}

template <class Key>
std::vector<async_uv::sql::SqlParam> keys_to_sql_params(const std::vector<Key>& keys) {
    std::vector<async_uv::sql::SqlParam> params;
    params.reserve(keys.size());
    for (const auto& key : keys) {
        params.push_back(to_sql_param(key));
    }
    return params;
}

struct TxFrame {
    std::optional<std::string> savepoint;
    bool rollback_only = false;
    int timeout_ms = 0;
    bool sqlite_query_only_enabled = false;
    bool sqlite_read_uncommitted_enabled = false;
};

struct TxState {
    std::vector<TxFrame> frames;
    std::size_t savepoint_seq = 0;
};

inline std::shared_ptr<TxState> shared_tx_state(async_uv::sql::Connection* connection) {
    static std::mutex mutex;
    static std::unordered_map<async_uv::sql::Connection*, std::weak_ptr<TxState>> states;

    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = states.find(connection); it != states.end()) {
        if (auto alive = it->second.lock()) {
            return alive;
        }
    }

    auto created = std::make_shared<TxState>();
    states[connection] = created;

    if (states.size() > 128) {
        for (auto it = states.begin(); it != states.end();) {
            if (it->second.expired()) {
                it = states.erase(it);
            } else {
                ++it;
            }
        }
    }
    return created;
}

} // namespace detail

template <class Model>
class Query {
private:
    template <class Member>
    static constexpr bool kIsMemberSelector =
        std::is_member_object_pointer_v<std::remove_cvref_t<Member>>
        || std::is_member_function_pointer_v<std::remove_cvref_t<Member>>;

    enum class PredicateKind {
        Binary,
        In,
        InSql,
        Between,
        NullCheck,
        Raw,
        Group,
    };

    enum class JoinKind {
        Inner,
        Left,
    };

    enum class BoolConnector {
        And,
        Or,
    };

    struct Predicate {
        PredicateKind kind = PredicateKind::Binary;
        BoolConnector connector = BoolConnector::And;
        std::string column;
        std::string op;
        std::vector<async_uv::sql::SqlParam> params;
        std::string raw_sql;
        std::vector<Predicate> children;
    };

    struct Order {
        std::string column;
        bool ascending = true;
    };

    struct Join {
        JoinKind kind = JoinKind::Inner;
        std::string table;
        std::string alias;
        std::string left_ref;
        std::string right_ref;
        std::string on_raw;
        bool raw_on = false;
    };

    struct SqlPlan {
        std::string sql;
        std::vector<async_uv::sql::SqlParam> params;
    };

    struct HavingClause {
        BoolConnector connector = BoolConnector::And;
        std::string sql;
        std::vector<async_uv::sql::SqlParam> params;
    };

public:
    Query() = default;

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& eq(Member member, Value&& value) {
        return add_binary(resolve_member_column(std::move(member)), "=", std::forward<Value>(value));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& ne(Member member, Value&& value) {
        return add_binary(resolve_member_column(std::move(member)), "!=", std::forward<Value>(value));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& gt(Member member, Value&& value) {
        return add_binary(resolve_member_column(std::move(member)), ">", std::forward<Value>(value));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& ge(Member member, Value&& value) {
        return add_binary(resolve_member_column(std::move(member)), ">=", std::forward<Value>(value));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& lt(Member member, Value&& value) {
        return add_binary(resolve_member_column(std::move(member)), "<", std::forward<Value>(value));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& le(Member member, Value&& value) {
        return add_binary(resolve_member_column(std::move(member)), "<=", std::forward<Value>(value));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& like(Member member, std::string pattern) {
        return add_binary(resolve_member_column(std::move(member)), "LIKE", "%" + pattern + "%");
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& like_raw(Member member, std::string pattern) {
        return add_binary(resolve_member_column(std::move(member)), "LIKE", std::move(pattern));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& likeRaw(Member member, std::string pattern) {
        return like_raw(std::move(member), std::move(pattern));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& is_null(Member member) {
        Predicate p;
        p.kind = PredicateKind::NullCheck;
        p.connector = consume_next_connector();
        p.column = resolve_member_column(std::move(member));
        p.op = "IS NULL";
        predicates_.push_back(std::move(p));
        return *this;
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& is_not_null(Member member) {
        Predicate p;
        p.kind = PredicateKind::NullCheck;
        p.connector = consume_next_connector();
        p.column = resolve_member_column(std::move(member));
        p.op = "IS NOT NULL";
        predicates_.push_back(std::move(p));
        return *this;
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& isNull(Member member) {
        return is_null(std::move(member));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& isNotNull(Member member) {
        return is_not_null(std::move(member));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& in(Member member, std::initializer_list<Value> values) {
        std::vector<async_uv::sql::SqlParam> params;
        params.reserve(values.size());
        for (const auto& value : values) {
            params.push_back(detail::to_sql_param(value));
        }
        return add_in(resolve_member_column(std::move(member)), std::move(params));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& in(Member member, std::vector<async_uv::sql::SqlParam> values) {
        return add_in(resolve_member_column(std::move(member)), std::move(values));
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& not_in(Member member, std::initializer_list<Value> values) {
        std::vector<async_uv::sql::SqlParam> params;
        params.reserve(values.size());
        for (const auto& value : values) {
            params.push_back(detail::to_sql_param(value));
        }
        return add_in(resolve_member_column(std::move(member)), std::move(params), "NOT IN");
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_in(Member member, std::vector<async_uv::sql::SqlParam> values) {
        return add_in(resolve_member_column(std::move(member)), std::move(values), "NOT IN");
    }

    template <class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& notIn(Member member, std::initializer_list<Value> values) {
        return not_in(std::move(member), values);
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& notIn(Member member, std::vector<async_uv::sql::SqlParam> values) {
        return not_in(std::move(member), std::move(values));
    }

    template <class Member, class V1, class V2>
        requires(kIsMemberSelector<Member>)
    Query& between(Member member, V1&& low, V2&& high) {
        return add_between(
            resolve_member_column(std::move(member)),
            std::forward<V1>(low),
            std::forward<V2>(high));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_like(Member member, std::string pattern) {
        return add_binary(resolve_member_column(std::move(member)), "NOT LIKE", "%" + pattern + "%");
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& notLike(Member member, std::string pattern) {
        return not_like(std::move(member), std::move(pattern));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& like_left(Member member, std::string pattern) {
        return add_binary(resolve_member_column(std::move(member)), "LIKE", "%" + pattern);
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& likeLeft(Member member, std::string pattern) {
        return like_left(std::move(member), std::move(pattern));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& like_right(Member member, std::string pattern) {
        return add_binary(resolve_member_column(std::move(member)), "LIKE", std::move(pattern) + "%");
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& likeRight(Member member, std::string pattern) {
        return like_right(std::move(member), std::move(pattern));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& in_sql(Member member, UnsafeSql sql_fragment) {
        return add_in_sql(
            resolve_member_column(std::move(member)),
            std::move(sql_fragment.sql),
            "IN",
            true);
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& in_sql(Member member, std::string sql_fragment) {
        return add_in_sql(
            resolve_member_column(std::move(member)),
            std::move(sql_fragment),
            "IN",
            false);
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_in_sql(Member member, UnsafeSql sql_fragment) {
        return add_in_sql(
            resolve_member_column(std::move(member)),
            std::move(sql_fragment.sql),
            "NOT IN",
            true);
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_in_sql(Member member, std::string sql_fragment) {
        return add_in_sql(
            resolve_member_column(std::move(member)),
            std::move(sql_fragment),
            "NOT IN",
            false);
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& inSql(Member member, UnsafeSql sql_fragment) {
        return in_sql(std::move(member), std::move(sql_fragment));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& inSql(Member member, std::string sql_fragment) {
        return in_sql(std::move(member), std::move(sql_fragment));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& notInSql(Member member, UnsafeSql sql_fragment) {
        return not_in_sql(std::move(member), std::move(sql_fragment));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& notInSql(Member member, std::string sql_fragment) {
        return not_in_sql(std::move(member), std::move(sql_fragment));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& eq(Member member, Value&& value) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "=",
            std::forward<Value>(value));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& ne(Member member, Value&& value) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "!=",
            std::forward<Value>(value));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& gt(Member member, Value&& value) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            ">",
            std::forward<Value>(value));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& ge(Member member, Value&& value) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            ">=",
            std::forward<Value>(value));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& lt(Member member, Value&& value) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "<",
            std::forward<Value>(value));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& le(Member member, Value&& value) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "<=",
            std::forward<Value>(value));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& like(Member member, std::string pattern) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "LIKE",
            "%" + pattern + "%");
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& like_raw(Member member, std::string pattern) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "LIKE",
            std::move(pattern));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& likeRaw(Member member, std::string pattern) {
        return like_raw<Joined>(std::move(member), std::move(pattern));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& is_null(Member member) {
        Predicate p;
        p.kind = PredicateKind::NullCheck;
        p.connector = consume_next_connector();
        p.column = resolve_joined_member_column_ref<Joined>(std::move(member));
        p.op = "IS NULL";
        predicates_.push_back(std::move(p));
        return *this;
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& is_not_null(Member member) {
        Predicate p;
        p.kind = PredicateKind::NullCheck;
        p.connector = consume_next_connector();
        p.column = resolve_joined_member_column_ref<Joined>(std::move(member));
        p.op = "IS NOT NULL";
        predicates_.push_back(std::move(p));
        return *this;
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& isNull(Member member) {
        return is_null<Joined>(std::move(member));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& isNotNull(Member member) {
        return is_not_null<Joined>(std::move(member));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& in(Member member, std::initializer_list<Value> values) {
        std::vector<async_uv::sql::SqlParam> params;
        params.reserve(values.size());
        for (const auto& value : values) {
            params.push_back(detail::to_sql_param(value));
        }
        return add_in(resolve_joined_member_column_ref<Joined>(std::move(member)), std::move(params));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& in(Member member, std::vector<async_uv::sql::SqlParam> values) {
        return add_in(resolve_joined_member_column_ref<Joined>(std::move(member)), std::move(values));
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& not_in(Member member, std::initializer_list<Value> values) {
        std::vector<async_uv::sql::SqlParam> params;
        params.reserve(values.size());
        for (const auto& value : values) {
            params.push_back(detail::to_sql_param(value));
        }
        return add_in(resolve_joined_member_column_ref<Joined>(std::move(member)), std::move(params), "NOT IN");
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_in(Member member, std::vector<async_uv::sql::SqlParam> values) {
        return add_in(resolve_joined_member_column_ref<Joined>(std::move(member)), std::move(values), "NOT IN");
    }

    template <class Joined, class Member, class Value>
        requires(kIsMemberSelector<Member>)
    Query& notIn(Member member, std::initializer_list<Value> values) {
        return not_in<Joined>(std::move(member), values);
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& notIn(Member member, std::vector<async_uv::sql::SqlParam> values) {
        return not_in<Joined>(std::move(member), std::move(values));
    }

    template <class Joined, class Member, class V1, class V2>
        requires(kIsMemberSelector<Member>)
    Query& between(Member member, V1&& low, V2&& high) {
        return add_between(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            std::forward<V1>(low),
            std::forward<V2>(high));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_like(Member member, std::string pattern) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "NOT LIKE",
            "%" + pattern + "%");
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& notLike(Member member, std::string pattern) {
        return not_like<Joined>(std::move(member), std::move(pattern));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& like_left(Member member, std::string pattern) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "LIKE",
            "%" + pattern);
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& likeLeft(Member member, std::string pattern) {
        return like_left<Joined>(std::move(member), std::move(pattern));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& like_right(Member member, std::string pattern) {
        return add_binary(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            "LIKE",
            std::move(pattern) + "%");
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& likeRight(Member member, std::string pattern) {
        return like_right<Joined>(std::move(member), std::move(pattern));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& in_sql(Member member, UnsafeSql sql_fragment) {
        return add_in_sql(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            std::move(sql_fragment.sql),
            "IN",
            true);
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& in_sql(Member member, std::string sql_fragment) {
        return add_in_sql(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            std::move(sql_fragment),
            "IN",
            false);
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_in_sql(Member member, UnsafeSql sql_fragment) {
        return add_in_sql(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            std::move(sql_fragment.sql),
            "NOT IN",
            true);
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& not_in_sql(Member member, std::string sql_fragment) {
        return add_in_sql(
            resolve_joined_member_column_ref<Joined>(std::move(member)),
            std::move(sql_fragment),
            "NOT IN",
            false);
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& inSql(Member member, UnsafeSql sql_fragment) {
        return in_sql<Joined>(std::move(member), std::move(sql_fragment));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& inSql(Member member, std::string sql_fragment) {
        return in_sql<Joined>(std::move(member), std::move(sql_fragment));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& notInSql(Member member, UnsafeSql sql_fragment) {
        return not_in_sql<Joined>(std::move(member), std::move(sql_fragment));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& notInSql(Member member, std::string sql_fragment) {
        return not_in_sql<Joined>(std::move(member), std::move(sql_fragment));
    }

    Query& from_alias(std::string alias) {
        if (alias.empty()) {
            table_alias_.reset();
        } else {
            table_alias_ = std::move(alias);
        }
        return *this;
    }

    Query& fromAlias(std::string alias) {
        return from_alias(std::move(alias));
    }

    Query& join_raw(std::string table, UnsafeSql on_sql, std::string alias = {}) {
        return add_join_raw(
            JoinKind::Inner,
            std::move(table),
            std::move(on_sql.sql),
            std::move(alias),
            true);
    }

    Query& join_raw(std::string table, std::string on_sql, std::string alias = {}) {
        return add_join_raw(
            JoinKind::Inner,
            std::move(table),
            std::move(on_sql),
            std::move(alias),
            false);
    }

    Query& left_join_raw(std::string table, UnsafeSql on_sql, std::string alias = {}) {
        return add_join_raw(
            JoinKind::Left,
            std::move(table),
            std::move(on_sql.sql),
            std::move(alias),
            true);
    }

    Query& left_join_raw(std::string table, std::string on_sql, std::string alias = {}) {
        return add_join_raw(
            JoinKind::Left,
            std::move(table),
            std::move(on_sql),
            std::move(alias),
            false);
    }

    Query& joinRaw(std::string table, UnsafeSql on_sql, std::string alias = {}) {
        return join_raw(std::move(table), std::move(on_sql), std::move(alias));
    }

    Query& joinRaw(std::string table, std::string on_sql, std::string alias = {}) {
        return join_raw(std::move(table), std::move(on_sql), std::move(alias));
    }

    Query& leftJoinRaw(std::string table, UnsafeSql on_sql, std::string alias = {}) {
        return left_join_raw(std::move(table), std::move(on_sql), std::move(alias));
    }

    Query& leftJoinRaw(std::string table, std::string on_sql, std::string alias = {}) {
        return left_join_raw(std::move(table), std::move(on_sql), std::move(alias));
    }

    template <class Joined, class LocalMember, class JoinedMember>
        requires(kIsMemberSelector<LocalMember> && kIsMemberSelector<JoinedMember>)
    Query& join(LocalMember local_member, JoinedMember joined_member, std::string alias = {}) {
        return add_typed_join<Joined>(
            JoinKind::Inner,
            std::move(local_member),
            std::move(joined_member),
            std::move(alias));
    }

    template <class Joined, class LocalMember, class JoinedMember>
        requires(kIsMemberSelector<LocalMember> && kIsMemberSelector<JoinedMember>)
    Query& left_join(LocalMember local_member, JoinedMember joined_member, std::string alias = {}) {
        return add_typed_join<Joined>(
            JoinKind::Left,
            std::move(local_member),
            std::move(joined_member),
            std::move(alias));
    }

    template <class Joined, class LocalMember, class JoinedMember>
        requires(kIsMemberSelector<LocalMember> && kIsMemberSelector<JoinedMember>)
    Query& leftJoin(LocalMember local_member, JoinedMember joined_member, std::string alias = {}) {
        return left_join<Joined>(
            std::move(local_member),
            std::move(joined_member),
            std::move(alias));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& order_by_asc(Member member) {
        orders_.push_back(Order{.column = resolve_member_column(std::move(member)), .ascending = true});
        return *this;
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& order_by_desc(Member member) {
        orders_.push_back(Order{.column = resolve_member_column(std::move(member)), .ascending = false});
        return *this;
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& order_by_asc(Member member) {
        orders_.push_back(Order{
            .column = resolve_joined_member_column_ref<Joined>(std::move(member)),
            .ascending = true});
        return *this;
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& order_by_desc(Member member) {
        orders_.push_back(Order{
            .column = resolve_joined_member_column_ref<Joined>(std::move(member)),
            .ascending = false});
        return *this;
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& orderByAsc(Member member) {
        return order_by_asc(std::move(member));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& orderByDesc(Member member) {
        return order_by_desc(std::move(member));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& orderByAsc(Member member) {
        return order_by_asc<Joined>(std::move(member));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& orderByDesc(Member member) {
        return order_by_desc<Joined>(std::move(member));
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& group_by(Member member) {
        group_by_columns_.push_back(resolve_member_column(std::move(member)));
        return *this;
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& group_by(Member member) {
        group_by_columns_.push_back(resolve_joined_member_column_ref<Joined>(std::move(member)));
        return *this;
    }

    template <class Member>
        requires(kIsMemberSelector<Member>)
    Query& groupBy(Member member) {
        return group_by(std::move(member));
    }

    template <class Joined, class Member>
        requires(kIsMemberSelector<Member>)
    Query& groupBy(Member member) {
        return group_by<Joined>(std::move(member));
    }

    Query& having(std::string sql_fragment, std::vector<async_uv::sql::SqlParam> params = {}) {
        if (sql_fragment.empty()) {
            return *this;
        }
        HavingClause clause;
        clause.connector = consume_next_connector();
        clause.sql = std::move(sql_fragment);
        clause.params = std::move(params);
        having_clauses_.push_back(std::move(clause));
        return *this;
    }

    Query& having(std::string sql_fragment, std::initializer_list<async_uv::sql::SqlParam> params) {
        return having(
            std::move(sql_fragment),
            std::vector<async_uv::sql::SqlParam>(params.begin(), params.end()));
    }

    Query& or_() {
        next_connector_ = BoolConnector::Or;
        return *this;
    }

    Query& and_() {
        next_connector_ = BoolConnector::And;
        return *this;
    }

    template <class Consumer>
        requires(std::invocable<Consumer, Query&>)
    Query& and_(Consumer&& consumer) {
        return append_group_predicate(BoolConnector::And, std::forward<Consumer>(consumer));
    }

    template <class Consumer>
        requires(std::invocable<Consumer, Query&>)
    Query& nested(Consumer&& consumer) {
        return append_group_predicate(consume_next_connector(), std::forward<Consumer>(consumer));
    }

    Query& last(std::string sql_tail) {
        sql_tail_ = std::move(sql_tail);
        return *this;
    }

    Query& limit(std::size_t count) {
        limit_ = count;
        return *this;
    }

    Query& offset(std::size_t count) {
        offset_ = count;
        return *this;
    }

    Query& where_raw(UnsafeSql sql_fragment, std::vector<async_uv::sql::SqlParam> params = {}) {
        return where_raw(std::move(sql_fragment.sql), std::move(params), true);
    }

    Query& where_raw(std::string sql_fragment, std::vector<async_uv::sql::SqlParam> params = {}) {
        return where_raw(std::move(sql_fragment), std::move(params), false);
    }

    Query& whereRaw(UnsafeSql sql_fragment, std::vector<async_uv::sql::SqlParam> params = {}) {
        return where_raw(std::move(sql_fragment), std::move(params));
    }

    Query& whereRaw(std::string sql_fragment, std::vector<async_uv::sql::SqlParam> params = {}) {
        return where_raw(std::move(sql_fragment), std::move(params));
    }

    Query without_window_clauses() const {
        Query copy = *this;
        copy.orders_.clear();
        copy.limit_.reset();
        copy.offset_.reset();
        copy.sql_tail_.clear();
        return copy;
    }

private:
    Query& where_raw(
        std::string sql_fragment,
        std::vector<async_uv::sql::SqlParam> params,
        bool unsafe_marked) {
        if (sql_fragment.empty()) {
            return *this;
        }
        detail::validate_raw_sql("Query::whereRaw", sql_fragment, unsafe_marked);
        Predicate p;
        p.kind = PredicateKind::Raw;
        p.connector = consume_next_connector();
        p.raw_sql = std::move(sql_fragment);
        p.params = std::move(params);
        predicates_.push_back(std::move(p));
        return *this;
    }
    BoolConnector consume_next_connector() {
        const auto c = next_connector_;
        next_connector_ = BoolConnector::And;
        return c;
    }

    template <class V>
    Query& add_binary(std::string column, std::string op, V&& value) {
        Predicate p;
        p.kind = PredicateKind::Binary;
        p.connector = consume_next_connector();
        p.column = std::move(column);
        p.op = std::move(op);
        p.params.push_back(detail::to_sql_param(std::forward<V>(value)));
        predicates_.push_back(std::move(p));
        return *this;
    }

    Query& add_in(
        std::string column,
        std::vector<async_uv::sql::SqlParam> values,
        std::string op = "IN") {
        Predicate p;
        p.kind = PredicateKind::In;
        p.connector = consume_next_connector();
        p.column = std::move(column);
        p.op = std::move(op);
        p.params = std::move(values);
        predicates_.push_back(std::move(p));
        return *this;
    }

    template <class V1, class V2>
    Query& add_between(std::string column, V1&& low, V2&& high) {
        Predicate p;
        p.kind = PredicateKind::Between;
        p.connector = consume_next_connector();
        p.column = std::move(column);
        p.params.push_back(detail::to_sql_param(std::forward<V1>(low)));
        p.params.push_back(detail::to_sql_param(std::forward<V2>(high)));
        predicates_.push_back(std::move(p));
        return *this;
    }

    Query& add_in_sql(
        std::string column,
        std::string sql_fragment,
        std::string op,
        bool unsafe_marked) {
        if (sql_fragment.empty()) {
            return *this;
        }
        detail::validate_raw_sql("Query::inSql", sql_fragment, unsafe_marked);
        Predicate p;
        p.kind = PredicateKind::InSql;
        p.connector = consume_next_connector();
        p.column = std::move(column);
        p.op = std::move(op);
        p.raw_sql = std::move(sql_fragment);
        predicates_.push_back(std::move(p));
        return *this;
    }

    template <class Consumer>
        requires(std::invocable<Consumer, Query&>)
    Query& append_group_predicate(BoolConnector connector, Consumer&& consumer) {
        Query nested_query;
        nested_query.table_alias_ = table_alias_;
        nested_query.joined_ref_by_type_ = joined_ref_by_type_;
        std::invoke(std::forward<Consumer>(consumer), nested_query);
        if (nested_query.predicates_.empty()) {
            return *this;
        }

        Predicate p;
        p.kind = PredicateKind::Group;
        p.connector = connector;
        p.children = std::move(nested_query.predicates_);
        predicates_.push_back(std::move(p));
        return *this;
    }

    Query& add_join(
        JoinKind kind,
        std::string table,
        std::string left_ref,
        std::string right_ref,
        std::string alias) {
        if (table.empty()) {
            throw Error("join table cannot be empty");
        }
        if (left_ref.empty() || right_ref.empty()) {
            throw Error("join column references cannot be empty");
        }
        Join join;
        join.kind = kind;
        join.table = std::move(table);
        join.alias = std::move(alias);
        join.left_ref = std::move(left_ref);
        join.right_ref = std::move(right_ref);
        joins_.push_back(std::move(join));
        return *this;
    }

    Query& add_join_raw(
        JoinKind kind,
        std::string table,
        std::string on_sql,
        std::string alias,
        bool unsafe_marked) {
        if (table.empty()) {
            throw Error("join table cannot be empty");
        }
        if (on_sql.empty()) {
            throw Error("join ON SQL cannot be empty");
        }
        detail::validate_raw_sql("Query::joinRaw", on_sql, unsafe_marked);
        Join join;
        join.kind = kind;
        join.table = std::move(table);
        join.alias = std::move(alias);
        join.raw_on = true;
        join.on_raw = std::move(on_sql);
        joins_.push_back(std::move(join));
        return *this;
    }

    template <class Joined, class LocalMember, class JoinedMember>
    Query& add_typed_join(
        JoinKind kind,
        LocalMember local_member,
        JoinedMember joined_member,
        std::string alias) {
        const auto joined_table = detail::resolve_table_name<Joined>();
        const auto joined_ref = alias.empty() ? joined_table : alias;
        joined_ref_by_type_[std::type_index(typeid(Joined))] = joined_ref;

        const auto local_column = resolve_member_column_for<Model>(std::move(local_member));
        const auto joined_column = resolve_member_column_for<Joined>(std::move(joined_member));
        return add_join(
            kind,
            joined_table,
            std::move(local_column),
            std::move(joined_column),
            std::move(alias));
    }

    static std::string render_column_ref(std::string_view column_ref, std::string_view default_table_ref) {
        return detail::qualify_and_quote_column(default_table_ref, column_ref);
    }

    template <class Member>
    static std::string resolve_member_column(Member member) {
        return resolve_member_column_for<Model>(std::move(member));
    }

    template <class AnyModel, class Member>
    static std::string resolve_member_column_for(Member member) {
        if constexpr (std::is_member_object_pointer_v<Member>) {
            return detail::resolve_column_name<AnyModel>(member);
        } else if constexpr (std::is_member_function_pointer_v<Member>) {
            return detail::resolve_column_name_from_getter<AnyModel>(member);
        } else {
            static_assert(detail::always_false_v<Member>, "unsupported member selector");
        }
    }

    template <class AnyModel, class Member>
    static std::string resolve_member_column_ref_for(Member member, std::string table_or_alias) {
        auto column = resolve_member_column_for<AnyModel>(std::move(member));
        if (table_or_alias.empty()) {
            table_or_alias = detail::resolve_table_name<AnyModel>();
        }
        std::string ref;
        ref.reserve(table_or_alias.size() + column.size() + 1);
        ref += table_or_alias;
        ref += ".";
        ref += column;
        return ref;
    }

    template <class Joined>
    std::string resolve_join_ref() const {
        const auto it = joined_ref_by_type_.find(std::type_index(typeid(Joined)));
        if (it != joined_ref_by_type_.end()) {
            return it->second;
        }
        return detail::resolve_table_name<Joined>();
    }

    template <class Joined, class Member>
    std::string resolve_joined_member_column_ref(Member member) const {
        return resolve_member_column_ref_for<Joined>(
            std::move(member),
            resolve_join_ref<Joined>());
    }

    bool append_predicates_sql(
        std::string& out_sql,
        std::vector<async_uv::sql::SqlParam>& out_params,
        std::string_view base_ref,
        const std::vector<Predicate>& predicates) const {
        bool has_any = false;

        for (const auto& p : predicates) {
            if (has_any) {
                out_sql += (p.connector == BoolConnector::Or ? " OR " : " AND ");
            }

            switch (p.kind) {
                case PredicateKind::Binary: {
                    out_sql += render_column_ref(p.column, base_ref);
                    out_sql += " ";
                    out_sql += p.op;
                    out_sql += " ?";
                    out_params.insert(out_params.end(), p.params.begin(), p.params.end());
                    has_any = true;
                    break;
                }
                case PredicateKind::In: {
                    out_sql += render_column_ref(p.column, base_ref);
                    out_sql += " ";
                    out_sql += (p.op.empty() ? "IN" : p.op);
                    if (p.params.empty()) {
                        out_sql += (p.op == "NOT IN" ? " (SELECT 1 WHERE 1=0)" : " (SELECT 1 WHERE 1=0)");
                    } else {
                        out_sql += " (";
                        for (std::size_t j = 0; j < p.params.size(); ++j) {
                            if (j > 0) {
                                out_sql += ", ";
                            }
                            out_sql += "?";
                        }
                        out_sql += ")";
                        out_params.insert(out_params.end(), p.params.begin(), p.params.end());
                    }
                    has_any = true;
                    break;
                }
                case PredicateKind::InSql: {
                    out_sql += render_column_ref(p.column, base_ref);
                    out_sql += " ";
                    out_sql += (p.op.empty() ? "IN" : p.op);
                    out_sql += " (";
                    out_sql += p.raw_sql;
                    out_sql += ")";
                    has_any = true;
                    break;
                }
                case PredicateKind::Between: {
                    if (p.params.size() < 2) {
                        out_sql += "1=0";
                    } else {
                        out_sql += render_column_ref(p.column, base_ref);
                        out_sql += " BETWEEN ? AND ?";
                        out_params.push_back(p.params[0]);
                        out_params.push_back(p.params[1]);
                    }
                    has_any = true;
                    break;
                }
                case PredicateKind::NullCheck: {
                    out_sql += render_column_ref(p.column, base_ref);
                    out_sql += " ";
                    out_sql += p.op;
                    has_any = true;
                    break;
                }
                case PredicateKind::Raw: {
                    out_sql += "(";
                    out_sql += p.raw_sql;
                    out_sql += ")";
                    out_params.insert(out_params.end(), p.params.begin(), p.params.end());
                    has_any = true;
                    break;
                }
                case PredicateKind::Group: {
                    std::string nested_sql;
                    std::vector<async_uv::sql::SqlParam> nested_params;
                    if (append_predicates_sql(nested_sql, nested_params, base_ref, p.children)) {
                        out_sql += "(";
                        out_sql += nested_sql;
                        out_sql += ")";
                        out_params.insert(
                            out_params.end(),
                            std::make_move_iterator(nested_params.begin()),
                            std::make_move_iterator(nested_params.end()));
                        has_any = true;
                    } else {
                        out_sql += "1=1";
                        has_any = true;
                    }
                    break;
                }
            }
        }

        return has_any;
    }

    Query with_limit_if_absent(std::size_t count) const {
        Query copy = *this;
        if (!copy.limit_.has_value()) {
            copy.limit_ = count;
        }
        return copy;
    }

    SqlPlan build_select_plan(
        std::string_view table,
        const std::vector<detail::ColumnMeta<Model>>& columns) const {
        SqlPlan plan;
        const std::string base_ref = table_alias_.has_value() ? *table_alias_ : std::string(table);

        plan.sql = "SELECT ";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                plan.sql += ", ";
            }
            plan.sql += render_column_ref(columns[i].name, base_ref);
            plan.sql += " AS ";
            plan.sql += detail::quote_ident(columns[i].name);
        }
        plan.sql += " FROM ";
        plan.sql += detail::quote_ident(table);
        if (table_alias_.has_value()) {
            plan.sql += " AS ";
            plan.sql += detail::quote_ident(*table_alias_);
        }

        for (const auto& join : joins_) {
            plan.sql += " ";
            plan.sql += (join.kind == JoinKind::Inner ? "INNER JOIN " : "LEFT JOIN ");
            plan.sql += detail::quote_ident(join.table);
            if (!join.alias.empty()) {
                plan.sql += " AS ";
                plan.sql += detail::quote_ident(join.alias);
            }
            plan.sql += " ON ";
            if (join.raw_on) {
                plan.sql += join.on_raw;
            } else {
                const std::string join_ref = join.alias.empty() ? join.table : join.alias;
                plan.sql += render_column_ref(join.left_ref, base_ref);
                plan.sql += " = ";
                plan.sql += render_column_ref(join.right_ref, join_ref);
            }
        }

        if (!predicates_.empty()) {
            std::string where_sql;
            if (append_predicates_sql(where_sql, plan.params, base_ref, predicates_)) {
                plan.sql += " WHERE ";
                plan.sql += where_sql;
            }
        }

        if (!group_by_columns_.empty()) {
            plan.sql += " GROUP BY ";
            for (std::size_t i = 0; i < group_by_columns_.size(); ++i) {
                if (i > 0) {
                    plan.sql += ", ";
                }
                plan.sql += render_column_ref(group_by_columns_[i], base_ref);
            }
        }

        if (!having_clauses_.empty()) {
            plan.sql += " HAVING ";
            for (std::size_t i = 0; i < having_clauses_.size(); ++i) {
                if (i > 0) {
                    plan.sql += having_clauses_[i].connector == BoolConnector::Or ? " OR " : " AND ";
                }
                plan.sql += "(";
                plan.sql += having_clauses_[i].sql;
                plan.sql += ")";
                plan.params.insert(
                    plan.params.end(),
                    having_clauses_[i].params.begin(),
                    having_clauses_[i].params.end());
            }
        }

        if (!orders_.empty()) {
            plan.sql += " ORDER BY ";
            for (std::size_t i = 0; i < orders_.size(); ++i) {
                if (i > 0) {
                    plan.sql += ", ";
                }
                plan.sql += render_column_ref(orders_[i].column, base_ref);
                plan.sql += orders_[i].ascending ? " ASC" : " DESC";
            }
        }

        if (limit_.has_value()) {
            plan.sql += " LIMIT ";
            plan.sql += std::to_string(*limit_);
        }
        if (offset_.has_value()) {
            plan.sql += " OFFSET ";
            plan.sql += std::to_string(*offset_);
        }

        if (!sql_tail_.empty()) {
            plan.sql += " ";
            plan.sql += sql_tail_;
        }

        return plan;
    }

private:
    std::vector<Predicate> predicates_;
    BoolConnector next_connector_ = BoolConnector::And;
    std::vector<Join> joins_;
    std::vector<Order> orders_;
    std::vector<std::string> group_by_columns_;
    std::vector<HavingClause> having_clauses_;
    std::unordered_map<std::type_index, std::string> joined_ref_by_type_;
    std::optional<std::string> table_alias_;
    std::optional<std::size_t> limit_;
    std::optional<std::size_t> offset_;
    std::string sql_tail_;

    template <class>
    friend class Mapper;
};

template <class Model>
class Mapper {
public:
    explicit Mapper(async_uv::sql::Connection& connection)
        : connection_(&connection),
          table_(detail::model_meta<Model>().table),
          tx_state_(detail::shared_tx_state(&connection)) {}

    Mapper(async_uv::sql::Connection& connection, std::string table_name)
        : connection_(&connection),
          table_(std::move(table_name)),
          tx_state_(detail::shared_tx_state(&connection)) {}

    const std::string& table() const noexcept {
        return table_;
    }

    bool in_transaction() const noexcept {
        return tx_state_ != nullptr && !tx_state_->frames.empty();
    }

    bool rollback_only() const noexcept {
        return in_transaction() && tx_state_->frames.back().rollback_only;
    }

    void set_rollback_only() {
        if (!in_transaction()) {
            throw Error("set_rollback_only requires an active transaction");
        }
        tx_state_->frames.back().rollback_only = true;
    }

    template <class Func>
    auto tx(Func&& func)
        -> Task<async_uv::TaskValue<std::invoke_result_t<Func, Mapper&>>> {
        co_return co_await tx(TxOptions{}, std::forward<Func>(func));
    }

    template <class Func>
    auto tx(TxOptions options, Func&& func)
        -> Task<async_uv::TaskValue<std::invoke_result_t<Func, Mapper&>>> {
        if (options.propagation == TxPropagation::requires_new) {
            co_return co_await tx_requires_new(options, std::forward<Func>(func));
        }
        co_return co_await tx_local(options, std::forward<Func>(func));
    }

    Task<void> sync_schema() {
        const auto& meta = detail::model_meta<Model>();

        std::string sql = "CREATE TABLE IF NOT EXISTS ";
        sql += detail::quote_ident(table_);
        sql += " (";

        for (std::size_t i = 0; i < meta.columns.size(); ++i) {
            const auto& col = meta.columns[i];
            if (i > 0) {
                sql += ", ";
            }
            sql += detail::quote_ident(col.name);
            sql += " ";
            sql += col.sql_type;

            if (col.is_id) {
                if (col.is_auto_increment) {
                    sql += " PRIMARY KEY AUTOINCREMENT";
                } else {
                    sql += " PRIMARY KEY";
                }
            }
            if (col.is_unique && !col.is_id) {
                sql += " UNIQUE";
            }
        }

        sql += ")";
        co_await execute_sql(std::move(sql));

        for (const auto& col : meta.columns) {
            if (!col.is_indexed || col.is_unique || col.is_id) {
                continue;
            }
            std::string idx_sql = "CREATE INDEX IF NOT EXISTS ";
            idx_sql += detail::quote_ident("idx_" + table_ + "_" + col.name);
            idx_sql += " ON ";
            idx_sql += detail::quote_ident(table_);
            idx_sql += "(";
            idx_sql += detail::quote_ident(col.name);
            idx_sql += ")";
            co_await execute_sql(std::move(idx_sql));
        }
    }

    Task<List<Model>> select_list(const Query<Model>& query) {
        const auto& meta = detail::model_meta<Model>();
        const auto plan = query.build_select_plan(table_, meta.columns);
        const auto result = co_await query_sql(plan.sql, plan.params);
        co_return decode_rows(result, meta);
    }

    Task<List<Model>> selectList(const Query<Model>& query) {
        co_return co_await select_list(query);
    }

    Task<std::uint64_t> count(const Query<Model>& query = {}) {
        co_return co_await count_filtered(query);
    }

    Task<std::uint64_t> count(const Query<Model>& query, CountMode mode) {
        switch (mode) {
            case CountMode::filtered:
                co_return co_await count_filtered(query);
            case CountMode::window:
                co_return co_await count_window(query);
        }
        co_return co_await count_filtered(query);
    }

    Task<std::uint64_t> count_filtered(const Query<Model>& query = {}) {
        const auto normalized = query.without_window_clauses();
        co_return co_await count_from_query(normalized);
    }

    Task<std::uint64_t> countFiltered(const Query<Model>& query = {}) {
        co_return co_await count_filtered(query);
    }

    Task<std::uint64_t> count_window(const Query<Model>& query = {}) {
        co_return co_await count_from_query(query);
    }

    Task<std::uint64_t> countWindow(const Query<Model>& query = {}) {
        co_return co_await count_window(query);
    }

private:
    Task<std::uint64_t> count_from_query(const Query<Model>& query) {
        const auto& meta = detail::model_meta<Model>();
        const auto plan = query.build_select_plan(table_, meta.columns);

        std::string sql = "SELECT COUNT(*) AS ";
        sql += detail::quote_ident("__orm_count");
        sql += " FROM (";
        sql += plan.sql;
        sql += ") AS ";
        sql += detail::quote_ident("__orm_count_sub");

        const auto result = co_await query_sql(std::move(sql), plan.params);
        if (result.rows.empty() || result.rows.front().values.empty() || !result.rows.front().values.front().has_value()) {
            co_return 0;
        }

        try {
            co_return static_cast<std::uint64_t>(std::stoull(*result.rows.front().values.front()));
        } catch (const std::exception&) {
            throw Error("failed to parse COUNT(*) result");
        }
    }

public:

    Task<std::optional<Model>> select_one(const Query<Model>& query) {
        const auto limited = query.with_limit_if_absent(1);
        auto rows = co_await select_list(limited);
        if (rows.empty()) {
            co_return std::nullopt;
        }
        co_return std::move(rows.front());
    }

    Task<std::optional<Model>> selectOne(const Query<Model>& query) {
        co_return co_await select_one(query);
    }

    Task<std::optional<Model>> select_by_id(async_uv::sql::SqlParam id) {
        const auto& meta = detail::model_meta<Model>();
        if (!meta.id_index.has_value()) {
            throw Error("select_by_id requires an id column");
        }

        Query<Model> query;
        query.where_raw(
                 detail::quote_ident(meta.columns[*meta.id_index].name) + " = ?",
                 {std::move(id)})
            .limit(1);
        co_return co_await select_one(query);
    }

    Task<std::optional<Model>> selectById(async_uv::sql::SqlParam id) {
        co_return co_await select_by_id(std::move(id));
    }

    template <class Related, class LocalKeyMember, class RelatedForeignKeyMember>
    Task<std::unordered_map<detail::member_key_t<Model, LocalKeyMember>, List<Related>>> load_one_to_many(
        const List<Model>& owners,
        LocalKeyMember local_key,
        Mapper<Related>& related_mapper,
        RelatedForeignKeyMember related_foreign_key,
        Query<Related> related_query = {}) {
        using OwnerKey = detail::member_key_t<Model, LocalKeyMember>;
        using RelatedForeignKey = detail::member_key_t<Related, RelatedForeignKeyMember>;

        static_assert(detail::is_hashable_key_v<OwnerKey>, "one-to-many key type must be hashable");
        static_assert(detail::is_hashable_key_v<RelatedForeignKey>, "one-to-many foreign key type must be hashable");

        std::unordered_map<OwnerKey, List<Related>> grouped;
        if (owners.empty()) {
            co_return grouped;
        }

        std::vector<OwnerKey> owner_keys;
        owner_keys.reserve(owners.size());
        for (const auto& owner : owners) {
            auto key = detail::extract_member_key(owner, local_key);
            if (key.has_value()) {
                owner_keys.push_back(std::move(*key));
            }
        }
        owner_keys = detail::dedupe_keys(std::move(owner_keys));
        if (owner_keys.empty()) {
            co_return grouped;
        }

        grouped.reserve(owner_keys.size());
        for (const auto& owner_key : owner_keys) {
            grouped.emplace(owner_key, List<Related>{});
        }

        related_query.in(related_foreign_key, detail::keys_to_sql_params(owner_keys));
        auto related_rows = co_await related_mapper.select_list(related_query);
        for (auto& row : related_rows) {
            auto foreign_key = detail::extract_member_key(row, related_foreign_key);
            if (!foreign_key.has_value()) {
                continue;
            }
            auto it = grouped.find(detail::cast_key<OwnerKey>(*foreign_key));
            if (it != grouped.end()) {
                it->second.push_back(std::move(row));
            }
        }

        co_return grouped;
    }

    template <class Related, class LocalKeyMember, class RelatedForeignKeyMember>
    Task<List<Related>> load_one_to_many(
        const Model& owner,
        LocalKeyMember local_key,
        Mapper<Related>& related_mapper,
        RelatedForeignKeyMember related_foreign_key,
        Query<Related> related_query = {}) {
        auto owner_key = detail::extract_member_key(owner, local_key);
        if (!owner_key.has_value()) {
            co_return List<Related>{};
        }

        auto grouped = co_await load_one_to_many(
            List<Model>{owner},
            std::move(local_key),
            related_mapper,
            std::move(related_foreign_key),
            std::move(related_query));
        const auto it = grouped.find(*owner_key);
        if (it == grouped.end()) {
            co_return List<Related>{};
        }
        co_return std::move(it->second);
    }

    template <class Related, class LocalKeyMember, class RelatedForeignKeyMember>
    Task<std::unordered_map<detail::member_key_t<Model, LocalKeyMember>, List<Related>>> loadOneToMany(
        const List<Model>& owners,
        LocalKeyMember local_key,
        Mapper<Related>& related_mapper,
        RelatedForeignKeyMember related_foreign_key,
        Query<Related> related_query = {}) {
        co_return co_await load_one_to_many(
            owners,
            std::move(local_key),
            related_mapper,
            std::move(related_foreign_key),
            std::move(related_query));
    }

    template <class Related, class LocalKeyMember, class RelatedForeignKeyMember>
    Task<List<Related>> loadOneToMany(
        const Model& owner,
        LocalKeyMember local_key,
        Mapper<Related>& related_mapper,
        RelatedForeignKeyMember related_foreign_key,
        Query<Related> related_query = {}) {
        co_return co_await load_one_to_many(
            owner,
            std::move(local_key),
            related_mapper,
            std::move(related_foreign_key),
            std::move(related_query));
    }

    template <class Parent, class ForeignKeyMember, class ParentKeyMember>
    Task<std::unordered_map<detail::member_key_t<Model, ForeignKeyMember>, Parent>> load_many_to_one(
        const List<Model>& children,
        ForeignKeyMember foreign_key,
        Mapper<Parent>& parent_mapper,
        ParentKeyMember parent_key,
        Query<Parent> parent_query = {}) {
        using ForeignKey = detail::member_key_t<Model, ForeignKeyMember>;
        using ParentKey = detail::member_key_t<Parent, ParentKeyMember>;

        static_assert(detail::is_hashable_key_v<ForeignKey>, "many-to-one foreign key type must be hashable");
        static_assert(detail::is_hashable_key_v<ParentKey>, "many-to-one parent key type must be hashable");

        std::unordered_map<ForeignKey, Parent> parent_by_foreign_key;
        if (children.empty()) {
            co_return parent_by_foreign_key;
        }

        std::vector<ForeignKey> foreign_keys;
        foreign_keys.reserve(children.size());
        for (const auto& child : children) {
            auto key = detail::extract_member_key(child, foreign_key);
            if (key.has_value()) {
                foreign_keys.push_back(std::move(*key));
            }
        }
        foreign_keys = detail::dedupe_keys(std::move(foreign_keys));
        if (foreign_keys.empty()) {
            co_return parent_by_foreign_key;
        }

        parent_query.in(parent_key, detail::keys_to_sql_params(foreign_keys));
        auto parents = co_await parent_mapper.select_list(parent_query);
        parent_by_foreign_key.reserve(parents.size());
        for (auto& parent : parents) {
            auto key = detail::extract_member_key(parent, parent_key);
            if (!key.has_value()) {
                continue;
            }
            parent_by_foreign_key[detail::cast_key<ForeignKey>(*key)] = std::move(parent);
        }

        co_return parent_by_foreign_key;
    }

    template <class Parent, class ForeignKeyMember, class ParentKeyMember>
    Task<std::optional<Parent>> load_many_to_one(
        const Model& child,
        ForeignKeyMember foreign_key,
        Mapper<Parent>& parent_mapper,
        ParentKeyMember parent_key,
        Query<Parent> parent_query = {}) {
        auto key = detail::extract_member_key(child, foreign_key);
        if (!key.has_value()) {
            co_return std::nullopt;
        }

        parent_query.eq(parent_key, *key).limit(1);
        co_return co_await parent_mapper.select_one(parent_query);
    }

    template <class Parent, class ForeignKeyMember, class ParentKeyMember>
    Task<std::unordered_map<detail::member_key_t<Model, ForeignKeyMember>, Parent>> loadManyToOne(
        const List<Model>& children,
        ForeignKeyMember foreign_key,
        Mapper<Parent>& parent_mapper,
        ParentKeyMember parent_key,
        Query<Parent> parent_query = {}) {
        co_return co_await load_many_to_one(
            children,
            std::move(foreign_key),
            parent_mapper,
            std::move(parent_key),
            std::move(parent_query));
    }

    template <class Parent, class ForeignKeyMember, class ParentKeyMember>
    Task<std::optional<Parent>> loadManyToOne(
        const Model& child,
        ForeignKeyMember foreign_key,
        Mapper<Parent>& parent_mapper,
        ParentKeyMember parent_key,
        Query<Parent> parent_query = {}) {
        co_return co_await load_many_to_one(
            child,
            std::move(foreign_key),
            parent_mapper,
            std::move(parent_key),
            std::move(parent_query));
    }

    template <
        class Related,
        class Join,
        class LocalKeyMember,
        class JoinLocalForeignKeyMember,
        class JoinRelatedForeignKeyMember,
        class RelatedKeyMember>
    Task<std::unordered_map<detail::member_key_t<Model, LocalKeyMember>, List<Related>>> load_many_to_many(
        const List<Model>& owners,
        LocalKeyMember local_key,
        Mapper<Join>& join_mapper,
        JoinLocalForeignKeyMember join_local_foreign_key,
        JoinRelatedForeignKeyMember join_related_foreign_key,
        Mapper<Related>& related_mapper,
        RelatedKeyMember related_key,
        Query<Related> related_query = {}) {
        using OwnerKey = detail::member_key_t<Model, LocalKeyMember>;
        using JoinOwnerKey = detail::member_key_t<Join, JoinLocalForeignKeyMember>;
        using JoinRelatedKey = detail::member_key_t<Join, JoinRelatedForeignKeyMember>;
        using RelatedKey = detail::member_key_t<Related, RelatedKeyMember>;

        static_assert(detail::is_hashable_key_v<OwnerKey>, "many-to-many owner key type must be hashable");
        static_assert(detail::is_hashable_key_v<JoinOwnerKey>, "many-to-many join owner key type must be hashable");
        static_assert(detail::is_hashable_key_v<JoinRelatedKey>, "many-to-many join related key type must be hashable");
        static_assert(detail::is_hashable_key_v<RelatedKey>, "many-to-many related key type must be hashable");

        std::unordered_map<OwnerKey, List<Related>> grouped;
        if (owners.empty()) {
            co_return grouped;
        }

        std::vector<OwnerKey> owner_keys;
        owner_keys.reserve(owners.size());
        for (const auto& owner : owners) {
            auto key = detail::extract_member_key(owner, local_key);
            if (key.has_value()) {
                owner_keys.push_back(std::move(*key));
            }
        }
        owner_keys = detail::dedupe_keys(std::move(owner_keys));
        if (owner_keys.empty()) {
            co_return grouped;
        }

        grouped.reserve(owner_keys.size());
        for (const auto& owner_key : owner_keys) {
            grouped.emplace(owner_key, List<Related>{});
        }

        Query<Join> join_query;
        join_query.in(join_local_foreign_key, detail::keys_to_sql_params(owner_keys));
        auto join_rows = co_await join_mapper.select_list(join_query);

        std::unordered_map<OwnerKey, std::vector<JoinRelatedKey>> related_keys_by_owner;
        related_keys_by_owner.reserve(owner_keys.size());

        std::unordered_set<JoinRelatedKey> related_key_set;
        std::vector<JoinRelatedKey> related_keys;
        related_keys.reserve(join_rows.size());
        for (const auto& join_row : join_rows) {
            auto owner_key = detail::extract_member_key(join_row, join_local_foreign_key);
            auto related_join_key = detail::extract_member_key(join_row, join_related_foreign_key);
            if (!owner_key.has_value() || !related_join_key.has_value()) {
                continue;
            }

            const auto normalized_owner_key = detail::cast_key<OwnerKey>(*owner_key);
            if (grouped.find(normalized_owner_key) == grouped.end()) {
                continue;
            }

            auto& bucket = related_keys_by_owner[normalized_owner_key];
            bucket.push_back(*related_join_key);
            if (related_key_set.insert(*related_join_key).second) {
                related_keys.push_back(*related_join_key);
            }
        }

        if (related_keys.empty()) {
            co_return grouped;
        }

        related_query.in(related_key, detail::keys_to_sql_params(related_keys));
        auto related_rows = co_await related_mapper.select_list(related_query);

        std::unordered_map<JoinRelatedKey, Related> related_by_key;
        related_by_key.reserve(related_rows.size());
        for (auto& row : related_rows) {
            auto key = detail::extract_member_key(row, related_key);
            if (!key.has_value()) {
                continue;
            }
            related_by_key[detail::cast_key<JoinRelatedKey>(*key)] = std::move(row);
        }

        for (auto& [owner_key, join_related_keys] : related_keys_by_owner) {
            auto grouped_it = grouped.find(owner_key);
            if (grouped_it == grouped.end()) {
                continue;
            }

            auto& bucket = grouped_it->second;
            bucket.reserve(join_related_keys.size());
            for (const auto& join_related_key : join_related_keys) {
                const auto it = related_by_key.find(join_related_key);
                if (it != related_by_key.end()) {
                    bucket.push_back(it->second);
                }
            }
        }

        co_return grouped;
    }

    template <
        class Related,
        class Join,
        class LocalKeyMember,
        class JoinLocalForeignKeyMember,
        class JoinRelatedForeignKeyMember,
        class RelatedKeyMember>
    Task<List<Related>> load_many_to_many(
        const Model& owner,
        LocalKeyMember local_key,
        Mapper<Join>& join_mapper,
        JoinLocalForeignKeyMember join_local_foreign_key,
        JoinRelatedForeignKeyMember join_related_foreign_key,
        Mapper<Related>& related_mapper,
        RelatedKeyMember related_key,
        Query<Related> related_query = {}) {
        auto owner_key = detail::extract_member_key(owner, local_key);
        if (!owner_key.has_value()) {
            co_return List<Related>{};
        }

        auto grouped = co_await load_many_to_many(
            List<Model>{owner},
            std::move(local_key),
            join_mapper,
            std::move(join_local_foreign_key),
            std::move(join_related_foreign_key),
            related_mapper,
            std::move(related_key),
            std::move(related_query));
        const auto it = grouped.find(*owner_key);
        if (it == grouped.end()) {
            co_return List<Related>{};
        }
        co_return std::move(it->second);
    }

    template <
        class Related,
        class Join,
        class LocalKeyMember,
        class JoinLocalForeignKeyMember,
        class JoinRelatedForeignKeyMember,
        class RelatedKeyMember>
    Task<std::unordered_map<detail::member_key_t<Model, LocalKeyMember>, List<Related>>> loadManyToMany(
        const List<Model>& owners,
        LocalKeyMember local_key,
        Mapper<Join>& join_mapper,
        JoinLocalForeignKeyMember join_local_foreign_key,
        JoinRelatedForeignKeyMember join_related_foreign_key,
        Mapper<Related>& related_mapper,
        RelatedKeyMember related_key,
        Query<Related> related_query = {}) {
        co_return co_await load_many_to_many(
            owners,
            std::move(local_key),
            join_mapper,
            std::move(join_local_foreign_key),
            std::move(join_related_foreign_key),
            related_mapper,
            std::move(related_key),
            std::move(related_query));
    }

    template <
        class Related,
        class Join,
        class LocalKeyMember,
        class JoinLocalForeignKeyMember,
        class JoinRelatedForeignKeyMember,
        class RelatedKeyMember>
    Task<List<Related>> loadManyToMany(
        const Model& owner,
        LocalKeyMember local_key,
        Mapper<Join>& join_mapper,
        JoinLocalForeignKeyMember join_local_foreign_key,
        JoinRelatedForeignKeyMember join_related_foreign_key,
        Mapper<Related>& related_mapper,
        RelatedKeyMember related_key,
        Query<Related> related_query = {}) {
        co_return co_await load_many_to_many(
            owner,
            std::move(local_key),
            join_mapper,
            std::move(join_local_foreign_key),
            std::move(join_related_foreign_key),
            related_mapper,
            std::move(related_key),
            std::move(related_query));
    }

    Task<std::uint64_t> insert(const Model& model) {
        const auto& meta = detail::model_meta<Model>();

        std::vector<std::string> columns;
        std::vector<async_uv::sql::SqlParam> params;

        for (const auto& col : meta.columns) {
            if (col.is_auto_increment && col.is_default(model)) {
                continue;
            }
            columns.push_back(col.name);
            params.push_back(col.read_param(model));
        }

        std::string sql;
        if (columns.empty()) {
            sql = "INSERT INTO ";
            sql += detail::quote_ident(table_);
            sql += " DEFAULT VALUES";
        } else {
            sql = "INSERT INTO ";
            sql += detail::quote_ident(table_);
            sql += " (";
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i > 0) {
                    sql += ", ";
                }
                sql += detail::quote_ident(columns[i]);
            }
            sql += ") VALUES (";
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i > 0) {
                    sql += ", ";
                }
                sql += "?";
            }
            sql += ")";
        }

        const auto result = co_await execute_sql(std::move(sql), std::move(params));
        co_return result.last_insert_id;
    }

    Task<std::uint64_t> update_by_query(
        const Query<Model>& query,
        std::vector<UpdateAssignment<Model>> assignments) {
        if (assignments.empty()) {
            co_return 0;
        }
        if (!query.joins_.empty() || !query.group_by_columns_.empty() || !query.having_clauses_.empty()) {
            throw Error("update_by_query does not support join/group/having");
        }
        if (query.limit_.has_value() || query.offset_.has_value()) {
            throw Error("update_by_query does not support limit/offset");
        }

        std::string sql = "UPDATE ";
        sql += detail::quote_ident(table_);
        sql += " SET ";

        std::vector<async_uv::sql::SqlParam> params;
        params.reserve(assignments.size() * 2 + query.predicates_.size() * 2);
        for (std::size_t i = 0; i < assignments.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            if (assignments[i].column.empty()) {
                throw Error("update assignment column cannot be empty");
            }
            if (assignments[i].sql_expression.empty()) {
                throw Error("update assignment expression cannot be empty");
            }
            sql += detail::quote_ident(assignments[i].column);
            sql += " = ";
            sql += assignments[i].sql_expression;
            params.insert(
                params.end(),
                std::make_move_iterator(assignments[i].params.begin()),
                std::make_move_iterator(assignments[i].params.end()));
        }

        if (!query.predicates_.empty()) {
            std::string where_sql;
            if (query.append_predicates_sql(where_sql, params, table_, query.predicates_)) {
                sql += " WHERE ";
                sql += where_sql;
            }
        }

        const auto result = co_await execute_sql(std::move(sql), std::move(params));
        co_return result.affected_rows;
    }

    Task<std::uint64_t> updateByQuery(
        const Query<Model>& query,
        std::vector<UpdateAssignment<Model>> assignments) {
        co_return co_await update_by_query(query, std::move(assignments));
    }

    Task<std::uint64_t> update_by_id(const Model& model) {
        const auto& meta = detail::model_meta<Model>();
        if (!meta.id_index.has_value()) {
            throw Error("update_by_id requires an id column");
        }

        const auto& id_col = meta.columns[*meta.id_index];

        std::string sql = "UPDATE ";
        sql += detail::quote_ident(table_);
        sql += " SET ";

        std::vector<async_uv::sql::SqlParam> params;
        bool has_any_set = false;
        for (std::size_t i = 0; i < meta.columns.size(); ++i) {
            if (i == *meta.id_index) {
                continue;
            }
            const auto& col = meta.columns[i];
            if (has_any_set) {
                sql += ", ";
            }
            sql += detail::quote_ident(col.name);
            sql += " = ?";
            params.push_back(col.read_param(model));
            has_any_set = true;
        }

        if (!has_any_set) {
            co_return 0;
        }

        sql += " WHERE ";
        sql += detail::quote_ident(id_col.name);
        sql += " = ?";
        params.push_back(id_col.read_param(model));

        const auto result = co_await execute_sql(std::move(sql), std::move(params));
        co_return result.affected_rows;
    }

    Task<std::uint64_t> updateById(const Model& model) {
        co_return co_await update_by_id(model);
    }

    Task<std::uint64_t> remove_by_id(async_uv::sql::SqlParam id) {
        const auto& meta = detail::model_meta<Model>();
        if (!meta.id_index.has_value()) {
            throw Error("remove_by_id requires an id column");
        }

        std::string sql = "DELETE FROM ";
        sql += detail::quote_ident(table_);
        sql += " WHERE ";
        sql += detail::quote_ident(meta.columns[*meta.id_index].name);
        sql += " = ?";

        const auto result = co_await execute_sql(std::move(sql), {std::move(id)});
        co_return result.affected_rows;
    }

    Task<std::uint64_t> removeById(async_uv::sql::SqlParam id) {
        co_return co_await remove_by_id(std::move(id));
    }

    Task<std::uint64_t> upsert(const Model& model) {
        const auto& meta = detail::model_meta<Model>();

        std::optional<std::size_t> conflict_index;
        if (meta.id_index.has_value()) {
            conflict_index = meta.id_index;
        } else if (!meta.unique_indexes.empty()) {
            conflict_index = meta.unique_indexes.front();
        }

        if (!conflict_index.has_value()) {
            co_return co_await insert(model);
        }

        const auto& conflict_col = meta.columns[*conflict_index];
        if (conflict_col.is_auto_increment && conflict_col.is_default(model)) {
            co_return co_await insert(model);
        }

        std::vector<std::string> columns;
        std::vector<async_uv::sql::SqlParam> params;
        for (const auto& col : meta.columns) {
            if (col.is_auto_increment && col.is_default(model)) {
                continue;
            }
            columns.push_back(col.name);
            params.push_back(col.read_param(model));
        }

        std::string sql = "INSERT INTO ";
        sql += detail::quote_ident(table_);
        sql += " (";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += detail::quote_ident(columns[i]);
        }
        sql += ") VALUES (";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += "?";
        }
        sql += ") ON CONFLICT(";
        sql += detail::quote_ident(conflict_col.name);
        sql += ") DO ";

        bool has_update = false;
        for (const auto& col : meta.columns) {
            if (col.name == conflict_col.name || col.is_id) {
                continue;
            }
            if (!has_update) {
                sql += "UPDATE SET ";
            } else {
                sql += ", ";
            }
            sql += detail::quote_ident(col.name);
            sql += " = excluded.";
            sql += detail::quote_ident(col.name);
            has_update = true;
        }

        if (!has_update) {
            sql += "NOTHING";
        }

        const auto result = co_await execute_sql(std::move(sql), std::move(params));
        co_return result.affected_rows;
    }

    Task<std::uint64_t> batch_insert(const List<Model>& models) {
        if (models.empty()) {
            co_return 0;
        }

        if (!in_transaction()) {
            co_return co_await tx([&](Mapper<Model>& scoped) -> Task<std::uint64_t> {
                co_return co_await scoped.batch_insert(models);
            });
        }

        std::uint64_t inserted = 0;
        for (const auto& model : models) {
            (void)co_await insert(model);
            ++inserted;
        }
        co_return inserted;
    }

    Task<std::uint64_t> batchInsert(const List<Model>& models) {
        co_return co_await batch_insert(models);
    }

    Task<std::uint64_t> batch_upsert(const List<Model>& models) {
        if (models.empty()) {
            co_return 0;
        }

        if (!in_transaction()) {
            co_return co_await tx([&](Mapper<Model>& scoped) -> Task<std::uint64_t> {
                co_return co_await scoped.batch_upsert(models);
            });
        }

        std::uint64_t affected = 0;
        for (const auto& model : models) {
            affected += co_await upsert(model);
        }
        co_return affected;
    }

    Task<std::uint64_t> batchUpsert(const List<Model>& models) {
        co_return co_await batch_upsert(models);
    }

    Task<std::uint64_t> batch_update_by_id(const List<Model>& models) {
        if (models.empty()) {
            co_return 0;
        }

        if (!in_transaction()) {
            co_return co_await tx([&](Mapper<Model>& scoped) -> Task<std::uint64_t> {
                co_return co_await scoped.batch_update_by_id(models);
            });
        }

        std::uint64_t affected = 0;
        for (const auto& model : models) {
            affected += co_await update_by_id(model);
        }
        co_return affected;
    }

    Task<std::uint64_t> batchUpdateById(const List<Model>& models) {
        co_return co_await batch_update_by_id(models);
    }

    Task<std::uint64_t> delete_by_query(const Query<Model>& query, bool allow_full_table = false) {
        if (!query.joins_.empty() || !query.group_by_columns_.empty() || !query.having_clauses_.empty()) {
            throw Error("delete_by_query does not support join/group/having");
        }
        if (!query.orders_.empty() || query.limit_.has_value() || query.offset_.has_value()) {
            throw Error("delete_by_query does not support order/limit/offset");
        }
        if (!query.sql_tail_.empty()) {
            throw Error("delete_by_query does not support SQL tail");
        }

        std::string sql = "DELETE FROM ";
        sql += detail::quote_ident(table_);

        std::vector<async_uv::sql::SqlParam> params;
        if (!query.predicates_.empty()) {
            std::string where_sql;
            if (query.append_predicates_sql(where_sql, params, table_, query.predicates_)) {
                sql += " WHERE ";
                sql += where_sql;
            }
        } else if (!allow_full_table) {
            throw Error(
                "delete_by_query without predicate is blocked; pass allow_full_table=true to delete all rows");
        }

        const auto result = co_await execute_sql(std::move(sql), std::move(params));
        co_return result.affected_rows;
    }

    Task<std::uint64_t> deleteByQuery(const Query<Model>& query, bool allow_full_table = false) {
        co_return co_await delete_by_query(query, allow_full_table);
    }

    QueryChain<Model> query_chain() {
        return QueryChain<Model>(*this);
    }

    QueryChain<Model> query() {
        return query_chain();
    }

    UpdateChain<Model> update_chain() {
        return UpdateChain<Model>(*this);
    }

    UpdateChain<Model> update() {
        return update_chain();
    }

private:
    template <class Func>
    auto tx_requires_new(TxOptions options, Func&& func)
        -> Task<async_uv::TaskValue<std::invoke_result_t<Func, Mapper&>>> {
        using Awaitable = std::invoke_result_t<Func, Mapper&>;
        using ReturnType = async_uv::TaskValue<Awaitable>;

        const auto conn_options = connection_->options();
        if (conn_options.driver == async_uv::sql::Driver::sqlite
            && (conn_options.file.empty() || conn_options.file == ":memory:")) {
            throw Error("requires_new needs a file-backed sqlite connection");
        }

        async_uv::sql::Connection isolated_conn;
        co_await isolated_conn.open(conn_options);
        Mapper<Model> isolated_mapper(isolated_conn, table_);

        TxOptions isolated_options = options;
        isolated_options.propagation = TxPropagation::required;

        std::exception_ptr op_error;
        if constexpr (std::is_void_v<ReturnType>) {
            try {
                co_await isolated_mapper.tx_local(
                    isolated_options,
                    [fn = std::forward<Func>(func)](Mapper<Model>& m) mutable -> Awaitable {
                        return std::invoke(std::move(fn), m);
                    });
            } catch (...) {
                op_error = std::current_exception();
            }

            try {
                co_await isolated_conn.close();
            } catch (...) {
                if (!op_error) {
                    op_error = std::current_exception();
                }
            }

            if (op_error) {
                std::rethrow_exception(op_error);
            }
            co_return;
        } else {
            std::optional<ReturnType> isolated_result;
            try {
                isolated_result = co_await isolated_mapper.tx_local(
                    isolated_options,
                    [fn = std::forward<Func>(func)](Mapper<Model>& m) mutable -> Awaitable {
                        return std::invoke(std::move(fn), m);
                    });
            } catch (...) {
                op_error = std::current_exception();
            }

            try {
                co_await isolated_conn.close();
            } catch (...) {
                if (!op_error) {
                    op_error = std::current_exception();
                }
            }

            if (op_error) {
                std::rethrow_exception(op_error);
            }
            co_return std::move(*isolated_result);
        }
    }

    template <class Func>
    auto tx_local(TxOptions options, Func&& func)
        -> Task<async_uv::TaskValue<std::invoke_result_t<Func, Mapper&>>> {
        using Awaitable = std::invoke_result_t<Func, Mapper&>;
        using ReturnType = async_uv::TaskValue<Awaitable>;

        if (options.propagation == TxPropagation::requires_new) {
            throw Error("requires_new is not allowed in local tx context");
        }

        const bool has_outer = !tx_state_->frames.empty();
        const bool has_tx_flags =
            options.read_only || options.isolation != TxIsolation::default_level;
        if (has_outer && has_tx_flags) {
            throw Error("cannot override isolation/read_only inside existing transaction; use requires_new");
        }

        const bool create_root = tx_state_->frames.empty();
        const bool create_savepoint =
            !create_root && options.propagation == TxPropagation::nested;
        const int inherited_timeout =
            tx_state_->frames.empty() ? 0 : tx_state_->frames.back().timeout_ms;
        const int effective_timeout_ms =
            options.timeout_ms > 0 ? options.timeout_ms : (create_savepoint ? inherited_timeout : 0);

        bool boundary_opened = false;
        if (create_root) {
            detail::TxFrame frame;
            frame.timeout_ms = effective_timeout_ms;

            bool began = false;
            std::exception_ptr begin_error;
            try {
                co_await execute_sql_with_timeout("BEGIN", frame.timeout_ms);
                began = true;
                co_await apply_root_tx_options(options, frame);
            } catch (...) {
                begin_error = std::current_exception();
            }

            if (begin_error) {
                if (began) {
                    try {
                        co_await execute_sql_with_timeout("ROLLBACK", frame.timeout_ms);
                    } catch (...) {
                    }
                }
                try {
                    co_await restore_root_tx_options(frame);
                } catch (...) {
                }
                std::rethrow_exception(begin_error);
            }

            tx_state_->frames.push_back(std::move(frame));
            boundary_opened = true;
        } else if (create_savepoint) {
            detail::TxFrame frame;
            frame.timeout_ms = effective_timeout_ms;
            frame.savepoint = "sp_" + std::to_string(++tx_state_->savepoint_seq);
            std::string save_sql = "SAVEPOINT ";
            save_sql += detail::quote_ident(*frame.savepoint);
            co_await execute_sql_with_timeout(std::move(save_sql), frame.timeout_ms);
            tx_state_->frames.push_back(std::move(frame));
            boundary_opened = true;
        }

        auto finalize_boundary = [&](detail::TxFrame frame) -> Task<void> {
            const bool is_root = !frame.savepoint.has_value();
            if (frame.rollback_only) {
                if (is_root) {
                    co_await execute_sql_with_timeout("ROLLBACK", frame.timeout_ms);
                } else {
                    std::string rollback_sql = "ROLLBACK TO SAVEPOINT ";
                    rollback_sql += detail::quote_ident(*frame.savepoint);
                    co_await execute_sql_with_timeout(std::move(rollback_sql), frame.timeout_ms);

                    std::string release_sql = "RELEASE SAVEPOINT ";
                    release_sql += detail::quote_ident(*frame.savepoint);
                    co_await execute_sql_with_timeout(std::move(release_sql), frame.timeout_ms);
                }
            } else {
                if (is_root) {
                    co_await execute_sql_with_timeout("COMMIT", frame.timeout_ms);
                } else {
                    std::string release_sql = "RELEASE SAVEPOINT ";
                    release_sql += detail::quote_ident(*frame.savepoint);
                    co_await execute_sql_with_timeout(std::move(release_sql), frame.timeout_ms);
                }
            }

            if (is_root) {
                co_await restore_root_tx_options(frame);
            }
        };

        std::exception_ptr body_error;
        bool body_failed = false;
        bool boundary_rollback = false;
        const int body_timeout_ms = boundary_opened ? effective_timeout_ms : options.timeout_ms;

        if constexpr (std::is_void_v<ReturnType>) {
            try {
                if (body_timeout_ms > 0) {
                    co_await async_uv::with_timeout(
                        std::chrono::milliseconds(body_timeout_ms),
                        std::invoke(std::forward<Func>(func), *this));
                } else {
                    co_await std::invoke(std::forward<Func>(func), *this);
                }
            } catch (...) {
                body_failed = true;
                try {
                    throw;
                } catch (const async_uv::Error& err) {
                    if (err.code() == UV_ETIMEDOUT) {
                        body_error = std::make_exception_ptr(Error("transaction timeout"));
                    } else {
                        body_error = std::current_exception();
                    }
                } catch (...) {
                    body_error = std::current_exception();
                }
            }

            if (boundary_opened) {
                if (!tx_state_->frames.empty() && body_failed) {
                    tx_state_->frames.back().rollback_only = true;
                }
                if (!tx_state_->frames.empty()) {
                    auto frame = std::move(tx_state_->frames.back());
                    tx_state_->frames.pop_back();
                    boundary_rollback = frame.rollback_only;
                    co_await finalize_boundary(std::move(frame));
                }
            } else if (body_failed && !tx_state_->frames.empty()) {
                tx_state_->frames.back().rollback_only = true;
            }

            if (body_failed) {
                std::rethrow_exception(body_error);
            }
            if (boundary_rollback) {
                throw Error("transaction marked rollback_only");
            }
            co_return;
        } else {
            std::optional<ReturnType> result;
            try {
                if (body_timeout_ms > 0) {
                    result = co_await async_uv::with_timeout(
                        std::chrono::milliseconds(body_timeout_ms),
                        std::invoke(std::forward<Func>(func), *this));
                } else {
                    result = co_await std::invoke(std::forward<Func>(func), *this);
                }
            } catch (...) {
                body_failed = true;
                try {
                    throw;
                } catch (const async_uv::Error& err) {
                    if (err.code() == UV_ETIMEDOUT) {
                        body_error = std::make_exception_ptr(Error("transaction timeout"));
                    } else {
                        body_error = std::current_exception();
                    }
                } catch (...) {
                    body_error = std::current_exception();
                }
            }

            if (boundary_opened) {
                if (!tx_state_->frames.empty() && body_failed) {
                    tx_state_->frames.back().rollback_only = true;
                }
                if (!tx_state_->frames.empty()) {
                    auto frame = std::move(tx_state_->frames.back());
                    tx_state_->frames.pop_back();
                    boundary_rollback = frame.rollback_only;
                    co_await finalize_boundary(std::move(frame));
                }
            } else if (body_failed && !tx_state_->frames.empty()) {
                tx_state_->frames.back().rollback_only = true;
            }

            if (body_failed) {
                std::rethrow_exception(body_error);
            }
            if (boundary_rollback) {
                throw Error("transaction marked rollback_only");
            }
            co_return std::move(*result);
        }
    }

    static async_uv::sql::QueryOptions make_query_options(int timeout_ms) {
        return async_uv::sql::QueryOptions::builder().timeout_ms(timeout_ms).build();
    }

    static std::string isolation_sql(TxIsolation isolation) {
        switch (isolation) {
            case TxIsolation::default_level:
                return "";
            case TxIsolation::read_uncommitted:
                return "READ UNCOMMITTED";
            case TxIsolation::read_committed:
                return "READ COMMITTED";
            case TxIsolation::repeatable_read:
                return "REPEATABLE READ";
            case TxIsolation::serializable:
                return "SERIALIZABLE";
        }
        return "";
    }

    std::optional<int> current_tx_timeout_ms() const noexcept {
        if (!in_transaction()) {
            return std::nullopt;
        }
        const int timeout_ms = tx_state_->frames.back().timeout_ms;
        if (timeout_ms <= 0) {
            return std::nullopt;
        }
        return timeout_ms;
    }

    Task<async_uv::sql::QueryResult> execute_sql(std::string sql) {
        if (const auto timeout_ms = current_tx_timeout_ms(); timeout_ms.has_value()) {
            co_return co_await connection_->execute(
                std::move(sql), make_query_options(*timeout_ms));
        }
        co_return co_await connection_->execute(std::move(sql));
    }

    Task<async_uv::sql::QueryResult> execute_sql(
        std::string sql, std::vector<async_uv::sql::SqlParam> params) {
        if (const auto timeout_ms = current_tx_timeout_ms(); timeout_ms.has_value()) {
            co_return co_await connection_->execute(
                std::move(sql), std::move(params), make_query_options(*timeout_ms));
        }
        co_return co_await connection_->execute(std::move(sql), std::move(params));
    }

    Task<async_uv::sql::QueryResult> execute_sql_with_timeout(std::string sql, int timeout_ms) {
        if (timeout_ms > 0) {
            co_return co_await connection_->execute(
                std::move(sql), make_query_options(timeout_ms));
        }
        co_return co_await connection_->execute(std::move(sql));
    }

    Task<async_uv::sql::QueryResult> query_sql(
        std::string sql, std::vector<async_uv::sql::SqlParam> params) {
        if (const auto timeout_ms = current_tx_timeout_ms(); timeout_ms.has_value()) {
            co_return co_await connection_->query(
                std::move(sql), std::move(params), make_query_options(*timeout_ms));
        }
        co_return co_await connection_->query(std::move(sql), std::move(params));
    }

    Task<void> apply_root_tx_options(TxOptions options, detail::TxFrame& frame) {
        if (!options.read_only && options.isolation == TxIsolation::default_level) {
            co_return;
        }

        const auto conn_options = connection_->options();
        const auto driver = conn_options.driver;

        if (driver == async_uv::sql::Driver::sqlite) {
            if (options.read_only) {
                co_await execute_sql_with_timeout("PRAGMA query_only = ON", frame.timeout_ms);
                frame.sqlite_query_only_enabled = true;
            }

            switch (options.isolation) {
                case TxIsolation::default_level:
                case TxIsolation::serializable:
                    break;
                case TxIsolation::read_uncommitted:
                    co_await execute_sql_with_timeout("PRAGMA read_uncommitted = 1", frame.timeout_ms);
                    frame.sqlite_read_uncommitted_enabled = true;
                    break;
                case TxIsolation::read_committed:
                case TxIsolation::repeatable_read:
                    throw Error("sqlite does not support requested transaction isolation level");
            }
            co_return;
        }

        if (options.isolation != TxIsolation::default_level) {
            auto iso = isolation_sql(options.isolation);
            if (iso.empty()) {
                throw Error("invalid transaction isolation level");
            }
            std::string sql = "SET TRANSACTION ISOLATION LEVEL ";
            sql += iso;
            co_await execute_sql_with_timeout(std::move(sql), frame.timeout_ms);
        }

        if (options.read_only) {
            co_await execute_sql_with_timeout("SET TRANSACTION READ ONLY", frame.timeout_ms);
        }
    }

    Task<void> restore_root_tx_options(const detail::TxFrame& frame) {
        if (!frame.sqlite_query_only_enabled && !frame.sqlite_read_uncommitted_enabled) {
            co_return;
        }

        const auto conn_options = connection_->options();
        if (conn_options.driver != async_uv::sql::Driver::sqlite) {
            co_return;
        }

        if (frame.sqlite_query_only_enabled) {
            co_await execute_sql_with_timeout("PRAGMA query_only = OFF", frame.timeout_ms);
        }
        if (frame.sqlite_read_uncommitted_enabled) {
            co_await execute_sql_with_timeout("PRAGMA read_uncommitted = 0", frame.timeout_ms);
        }
    }

    static List<Model> decode_rows(
        const async_uv::sql::QueryResult& result,
        const detail::ModelMeta<Model>& meta) {
        std::unordered_map<std::string, std::size_t> column_pos;
        column_pos.reserve(result.columns.size());
        for (std::size_t i = 0; i < result.columns.size(); ++i) {
            column_pos[result.columns[i]] = i;
        }

        List<Model> out;
        out.reserve(result.rows.size());

        for (const auto& row : result.rows) {
            Model model{};
            for (const auto& col : meta.columns) {
                const auto it = column_pos.find(col.name);
                if (it == column_pos.end()) {
                    continue;
                }
                if (it->second >= row.values.size()) {
                    throw Error("row column index out of range while decoding ORM row");
                }
                col.write_cell(model, row.values[it->second]);
            }
            out.push_back(std::move(model));
        }

        return out;
    }

private:
    async_uv::sql::Connection* connection_ = nullptr;
    std::string table_;
    std::shared_ptr<detail::TxState> tx_state_;
};

template <class Model>
class QueryChain {
public:
    explicit QueryChain(Mapper<Model>& mapper) : mapper_(&mapper) {}

    template <class... Args>
    QueryChain& eq(Args&&... args) {
        query_.eq(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& ne(Args&&... args) {
        query_.ne(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& gt(Args&&... args) {
        query_.gt(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& ge(Args&&... args) {
        query_.ge(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& lt(Args&&... args) {
        query_.lt(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& le(Args&&... args) {
        query_.le(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& between(Args&&... args) {
        query_.between(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& like(Args&&... args) {
        query_.like(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& notLike(Args&&... args) {
        query_.notLike(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& likeLeft(Args&&... args) {
        query_.likeLeft(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& likeRight(Args&&... args) {
        query_.likeRight(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& in(Args&&... args) {
        query_.in(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& notIn(Args&&... args) {
        query_.notIn(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& inSql(Args&&... args) {
        query_.inSql(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& isNull(Args&&... args) {
        query_.isNull(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& isNotNull(Args&&... args) {
        query_.isNotNull(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& join(Args&&... args) {
        query_.join(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& leftJoin(Args&&... args) {
        query_.leftJoin(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& joinRaw(Args&&... args) {
        query_.joinRaw(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& leftJoinRaw(Args&&... args) {
        query_.leftJoinRaw(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& groupBy(Args&&... args) {
        query_.groupBy(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& having(Args&&... args) {
        query_.having(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& orderByAsc(Args&&... args) {
        query_.orderByAsc(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& orderByDesc(Args&&... args) {
        query_.orderByDesc(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    QueryChain& whereRaw(Args&&... args) {
        query_.whereRaw(std::forward<Args>(args)...);
        return *this;
    }

    QueryChain& or_() {
        query_.or_();
        return *this;
    }

    QueryChain& and_() {
        query_.and_();
        return *this;
    }

    template <class Consumer>
    QueryChain& and_(Consumer&& consumer) {
        query_.and_(std::forward<Consumer>(consumer));
        return *this;
    }

    template <class Consumer>
    QueryChain& nested(Consumer&& consumer) {
        query_.nested(std::forward<Consumer>(consumer));
        return *this;
    }

    QueryChain& limit(std::size_t count) {
        query_.limit(count);
        return *this;
    }

    QueryChain& offset(std::size_t count) {
        query_.offset(count);
        return *this;
    }

    QueryChain& last(std::string sql_tail) {
        query_.last(std::move(sql_tail));
        return *this;
    }

    Task<List<Model>> list() const {
        co_return co_await mapper_->selectList(query_);
    }

    Task<std::optional<Model>> one() const {
        co_return co_await mapper_->selectOne(query_);
    }

    Task<std::uint64_t> count() const {
        co_return co_await mapper_->count_filtered(query_);
    }

    Task<std::uint64_t> countFiltered() const {
        co_return co_await mapper_->count_filtered(query_);
    }

    Task<std::uint64_t> countWindow() const {
        co_return co_await mapper_->count_window(query_);
    }

    const Query<Model>& raw_query() const noexcept {
        return query_;
    }

private:
    Mapper<Model>* mapper_ = nullptr;
    Query<Model> query_;
};

template <class Model>
class UpdateChain {
public:
    explicit UpdateChain(Mapper<Model>& mapper) : mapper_(&mapper) {}

    template <class Member, class Value>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& set(Member member, Value&& value) {
        auto column = detail::resolve_column_name<Model>(member);
        assignments_.push_back({
            .column = std::move(column),
            .sql_expression = "?",
            .params = {detail::to_sql_param(std::forward<Value>(value))}});
        assigned_columns_.insert(assignments_.back().column);
        return *this;
    }

    template <class Member, class Value>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setIf(bool condition, Member member, Value&& value) {
        if (condition) {
            set(std::move(member), std::forward<Value>(value));
        }
        return *this;
    }

    template <class Member, class Value>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setIfAbsent(Member member, Value&& value) {
        auto column = detail::resolve_column_name<Model>(member);
        if (assigned_columns_.contains(column)) {
            return *this;
        }
        assignments_.push_back({
            .column = column,
            .sql_expression = "?",
            .params = {detail::to_sql_param(std::forward<Value>(value))}});
        assigned_columns_.insert(std::move(column));
        return *this;
    }

    template <class Member>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setSql(
        Member member,
        UnsafeSql sql_expression,
        std::vector<async_uv::sql::SqlParam> params = {}) {
        return setSql(
            std::move(member),
            std::move(sql_expression.sql),
            std::move(params),
            true);
    }

    template <class Member>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setSql(
        Member member,
        std::string sql_expression,
        std::vector<async_uv::sql::SqlParam> params = {}) {
        return setSql(
            std::move(member),
            std::move(sql_expression),
            std::move(params),
            false);
    }

    template <class Member>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setSql(
        Member member,
        UnsafeSql sql_expression,
        std::initializer_list<async_uv::sql::SqlParam> params) {
        return setSql(
            std::move(member),
            std::move(sql_expression.sql),
            std::vector<async_uv::sql::SqlParam>(params.begin(), params.end()),
            true);
    }

    template <class Member>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setSql(
        Member member,
        std::string sql_expression,
        std::initializer_list<async_uv::sql::SqlParam> params) {
        return setSql(
            std::move(member),
            std::move(sql_expression),
            std::vector<async_uv::sql::SqlParam>(params.begin(), params.end()),
            false);
    }

private:
    template <class Member>
        requires(std::is_member_object_pointer_v<std::remove_cvref_t<Member>>)
    UpdateChain& setSql(
        Member member,
        std::string sql_expression,
        std::vector<async_uv::sql::SqlParam> params,
        bool unsafe_marked) {
        if (sql_expression.empty()) {
            throw Error("setSql expression cannot be empty");
        }
        detail::validate_raw_sql("UpdateChain::setSql", sql_expression, unsafe_marked);
        auto column = detail::resolve_column_name<Model>(member);
        assignments_.push_back({
            .column = std::move(column),
            .sql_expression = std::move(sql_expression),
            .params = std::move(params)});
        assigned_columns_.insert(assignments_.back().column);
        return *this;
    }

public:
    template <class... Args>
    UpdateChain& eq(Args&&... args) {
        query_.eq(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& ne(Args&&... args) {
        query_.ne(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& gt(Args&&... args) {
        query_.gt(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& ge(Args&&... args) {
        query_.ge(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& lt(Args&&... args) {
        query_.lt(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& le(Args&&... args) {
        query_.le(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& between(Args&&... args) {
        query_.between(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& like(Args&&... args) {
        query_.like(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& notLike(Args&&... args) {
        query_.notLike(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& likeLeft(Args&&... args) {
        query_.likeLeft(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& likeRight(Args&&... args) {
        query_.likeRight(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& in(Args&&... args) {
        query_.in(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& notIn(Args&&... args) {
        query_.notIn(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& inSql(Args&&... args) {
        query_.inSql(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& isNull(Args&&... args) {
        query_.isNull(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& isNotNull(Args&&... args) {
        query_.isNotNull(std::forward<Args>(args)...);
        return *this;
    }

    template <class... Args>
    UpdateChain& whereRaw(Args&&... args) {
        query_.whereRaw(std::forward<Args>(args)...);
        return *this;
    }

    UpdateChain& or_() {
        query_.or_();
        return *this;
    }

    UpdateChain& and_() {
        query_.and_();
        return *this;
    }

    template <class Consumer>
    UpdateChain& and_(Consumer&& consumer) {
        query_.and_(std::forward<Consumer>(consumer));
        return *this;
    }

    template <class Consumer>
    UpdateChain& nested(Consumer&& consumer) {
        query_.nested(std::forward<Consumer>(consumer));
        return *this;
    }

    Task<std::uint64_t> execute() {
        co_return co_await mapper_->update_by_query(query_, std::move(assignments_));
    }

    const Query<Model>& raw_query() const noexcept {
        return query_;
    }

private:
    Mapper<Model>* mapper_ = nullptr;
    Query<Model> query_;
    std::vector<UpdateAssignment<Model>> assignments_;
    std::unordered_set<std::string> assigned_columns_;
};

template <class Model>
class Service {
public:
    explicit Service(Mapper<Model>& mapper) : mapper_(&mapper) {}

    explicit Service(async_uv::sql::Connection& connection)
        : owned_mapper_(std::in_place, connection), mapper_(&*owned_mapper_) {}

    Service(async_uv::sql::Connection& connection, std::string table_name)
        : owned_mapper_(std::in_place, connection, std::move(table_name)), mapper_(&*owned_mapper_) {}

    Mapper<Model>& mapper() {
        return *mapper_;
    }

    const Mapper<Model>& mapper() const {
        return *mapper_;
    }

    bool in_transaction() const noexcept {
        return mapper_->in_transaction();
    }

    bool rollback_only() const noexcept {
        return mapper_->rollback_only();
    }

    void set_rollback_only() {
        mapper_->set_rollback_only();
    }

    template <class Func>
    auto tx(Func&& func)
        -> Task<async_uv::TaskValue<std::invoke_result_t<Func, Service&>>> {
        co_return co_await tx(TxOptions{}, std::forward<Func>(func));
    }

    template <class Func>
    auto tx(TxOptions options, Func&& func)
        -> Task<async_uv::TaskValue<std::invoke_result_t<Func, Service&>>> {
        using Awaitable = std::invoke_result_t<Func, Service&>;
        using ReturnType = async_uv::TaskValue<Awaitable>;

        if constexpr (std::is_void_v<ReturnType>) {
            co_await mapper_->tx(
                options,
                [fn = std::forward<Func>(func)](Mapper<Model>& active_mapper) mutable -> Awaitable {
                    Service<Model> scoped(active_mapper);
                    co_await std::invoke(std::move(fn), scoped);
                    co_return;
                });
            co_return;
        } else {
            ReturnType result = co_await mapper_->tx(
                options,
                [fn = std::forward<Func>(func)](Mapper<Model>& active_mapper) mutable -> Awaitable {
                    Service<Model> scoped(active_mapper);
                    co_return co_await std::invoke(std::move(fn), scoped);
                });
            co_return result;
        }
    }

    QueryChain<Model> query() {
        return QueryChain<Model>(*mapper_);
    }

    UpdateChain<Model> update() {
        return UpdateChain<Model>(*mapper_);
    }

    Task<List<Model>> list(const Query<Model>& query_model = {}) {
        co_return co_await mapper_->selectList(query_model);
    }

    Task<std::optional<Model>> one(const Query<Model>& query_model) {
        co_return co_await mapper_->selectOne(query_model);
    }

    Task<std::uint64_t> count(const Query<Model>& query_model = {}) {
        co_return co_await mapper_->count_filtered(query_model);
    }

    Task<std::uint64_t> count(const Query<Model>& query_model, CountMode mode) {
        co_return co_await mapper_->count(query_model, mode);
    }

    Task<std::uint64_t> count_filtered(const Query<Model>& query_model = {}) {
        co_return co_await mapper_->count_filtered(query_model);
    }

    Task<std::uint64_t> countFiltered(const Query<Model>& query_model = {}) {
        co_return co_await mapper_->count_filtered(query_model);
    }

    Task<std::uint64_t> count_window(const Query<Model>& query_model = {}) {
        co_return co_await mapper_->count_window(query_model);
    }

    Task<std::uint64_t> countWindow(const Query<Model>& query_model = {}) {
        co_return co_await mapper_->count_window(query_model);
    }

    Task<std::uint64_t> insert(const Model& model) {
        co_return co_await mapper_->insert(model);
    }

    Task<std::uint64_t> upsert(const Model& model) {
        co_return co_await mapper_->upsert(model);
    }

    Task<std::uint64_t> batch_insert(const List<Model>& models) {
        co_return co_await mapper_->batch_insert(models);
    }

    Task<std::uint64_t> batchInsert(const List<Model>& models) {
        co_return co_await mapper_->batch_insert(models);
    }

    Task<std::uint64_t> batch_upsert(const List<Model>& models) {
        co_return co_await mapper_->batch_upsert(models);
    }

    Task<std::uint64_t> batchUpsert(const List<Model>& models) {
        co_return co_await mapper_->batch_upsert(models);
    }

    Task<std::uint64_t> batch_update_by_id(const List<Model>& models) {
        co_return co_await mapper_->batch_update_by_id(models);
    }

    Task<std::uint64_t> batchUpdateById(const List<Model>& models) {
        co_return co_await mapper_->batch_update_by_id(models);
    }

    Task<std::uint64_t> delete_by_query(const Query<Model>& query_model, bool allow_full_table = false) {
        co_return co_await mapper_->delete_by_query(query_model, allow_full_table);
    }

    Task<std::uint64_t> deleteByQuery(const Query<Model>& query_model, bool allow_full_table = false) {
        co_return co_await mapper_->delete_by_query(query_model, allow_full_table);
    }

private:
    std::optional<Mapper<Model>> owned_mapper_;
    Mapper<Model>* mapper_ = nullptr;
};

} // namespace async_uv::layer3::orm
