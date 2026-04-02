#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace async_uv::layer3 {

template<typename Handler>
class RouterNode {
public:
    enum class Type : uint8_t {
        STATIC,
        PARAM,
        WILDCARD
    };

    std::string prefix;
    Type type = Type::STATIC;
    std::string param_name;
    
    std::vector<std::unique_ptr<RouterNode>> children;
    std::unordered_map<std::string, Handler> handlers;

    RouterNode() = default;
    explicit RouterNode(std::string p, Type t = Type::STATIC, std::string name = "")
        : prefix(std::move(p)), type(t), param_name(std::move(name)) {}

    static constexpr char PARAM_CHAR = '{';
    static constexpr char WILDCARD_CHAR = '*';

    static constexpr Type classify(std::string_view segment) {
        if (segment.empty()) {
            return Type::STATIC;
        }
        if (segment[0] != PARAM_CHAR) {
            return Type::STATIC;
        }

        // Canonical syntax: {name} / {name*}
        if (segment.size() >= 2 && segment.back() == '}') {
            const std::string_view inner = segment.substr(1, segment.size() - 2);
            if (!inner.empty() && inner.back() == '*') {
                return Type::WILDCARD;
            }
            return Type::PARAM;
        }

        // Backward-compatible fallback: "{name*"
        if (segment.size() > 1 && segment.back() == '*') {
            return Type::WILDCARD;
        }
        return Type::PARAM;
    }

    static constexpr std::pair<std::string_view, std::string_view> parse_param_segment(std::string_view segment) {
        if (segment.empty() || segment[0] != PARAM_CHAR) {
            return {segment, ""};
        }

        // Canonical syntax: {name} / {name*}
        if (segment.size() >= 2 && segment.back() == '}') {
            std::string_view inner = segment.substr(1, segment.size() - 2);
            if (!inner.empty() && inner.back() == '*') {
                inner = inner.substr(0, inner.size() - 1);
            }
            return {"", inner};
        }

        // Backward-compatible fallback: "{name*"
        if (segment.size() > 1 && segment.back() == '*') {
            return {"", segment.substr(1, segment.size() - 2)};
        }

        const size_t end = segment.find('}');
        if (end == std::string_view::npos) {
            return {segment, ""};
        }
        return {"", segment.substr(1, end - 1)};
    }

    RouterNode* add_child(std::string_view path, Type type, std::string param_name = "") {
        for (auto& child : children) {
            if (child->type != type) continue;

            if (type == Type::PARAM || type == Type::WILDCARD) {
                if (child->param_name.empty() && !param_name.empty()) {
                    child->param_name = param_name;
                }
                return child.get();
            }
            
            if (type == Type::STATIC) {
                size_t common = common_prefix(child->prefix, path);
                if (common > 0) {
                    if (common < child->prefix.size()) {
                        auto split = std::make_unique<RouterNode>(
                            child->prefix.substr(common), child->type, child->param_name);
                        split->children = std::move(child->children);
                        split->handlers = std::move(child->handlers);
                        
                        child->prefix = child->prefix.substr(0, common);
                        child->type = Type::STATIC;
                        child->param_name.clear();
                        child->children.clear();
                        child->handlers.clear();
                        
                        child->children.push_back(std::move(split));
                    }
                    
                    path = path.substr(common);
                    if (path.empty()) {
                        return child.get();
                    }
                    continue;
                }
            }
        }
        
        auto new_node = std::make_unique<RouterNode>(std::string(path), type, std::move(param_name));
        children.push_back(std::move(new_node));
        return children.back().get();
    }

    struct MatchResult {
        Handler handler;
        std::vector<std::pair<std::string, std::string>> params;
    };

    std::optional<MatchResult> match(std::string_view method, std::string_view path) const {
        std::vector<std::pair<std::string, std::string>> params;
        const RouterNode* node = this;
        std::string_view remaining = path;
        
        while (!remaining.empty()) {
            if (!remaining.empty() && remaining.front() == '/') {
                remaining = remaining.substr(1);
                continue;
            }

            auto try_match = [&](Type expected) -> bool {
                for (const auto& child : node->children) {
                    if (child->type != expected) {
                        continue;
                    }

                    if (expected == Type::STATIC) {
                        if (remaining.size() >= child->prefix.size()
                            && remaining.substr(0, child->prefix.size()) == child->prefix) {
                            remaining = remaining.substr(child->prefix.size());
                            node = child.get();
                            return true;
                        }
                        continue;
                    }

                    if (expected == Type::PARAM) {
                        size_t slash_pos = remaining.find('/');
                        std::string_view value = (slash_pos == std::string_view::npos)
                            ? remaining : remaining.substr(0, slash_pos);
                        if (!value.empty()) {
                            params.emplace_back(child->param_name, std::string(value));
                            remaining = (slash_pos == std::string_view::npos)
                                ? std::string_view{} : remaining.substr(slash_pos);
                            node = child.get();
                            return true;
                        }
                        continue;
                    }

                    params.emplace_back(child->param_name, std::string(remaining));
                    remaining = {};
                    node = child.get();
                    return true;
                }
                return false;
            };

            bool matched = try_match(Type::STATIC) || try_match(Type::PARAM) || try_match(Type::WILDCARD);
            if (!matched) {
                break;
            }
        }
        
        if (remaining.empty()) {
            std::string method_key(method);
            auto it = node->handlers.find(method_key);
            if (it != node->handlers.end()) {
                return MatchResult{it->second, std::move(params)};
            }

            it = node->handlers.find("*");
            if (it != node->handlers.end()) {
                return MatchResult{it->second, std::move(params)};
            }
        }

        return std::nullopt;
    }

    const RouterNode* match_node(std::string_view path) const {
        const RouterNode* node = this;
        std::string_view remaining = path;

        while (!remaining.empty()) {
            if (!remaining.empty() && remaining.front() == '/') {
                remaining = remaining.substr(1);
                continue;
            }

            bool matched = false;

            for (const auto& child : node->children) {
                if (child->type != Type::STATIC) {
                    continue;
                }
                if (remaining.size() >= child->prefix.size()
                    && remaining.substr(0, child->prefix.size()) == child->prefix) {
                    remaining = remaining.substr(child->prefix.size());
                    node = child.get();
                    matched = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }

            for (const auto& child : node->children) {
                if (child->type != Type::PARAM) {
                    continue;
                }
                size_t slash_pos = remaining.find('/');
                std::string_view value = (slash_pos == std::string_view::npos)
                    ? remaining : remaining.substr(0, slash_pos);
                if (value.empty()) {
                    continue;
                }

                remaining = (slash_pos == std::string_view::npos)
                    ? std::string_view{} : remaining.substr(slash_pos);
                node = child.get();
                matched = true;
                break;
            }
            if (matched) {
                continue;
            }

            for (const auto& child : node->children) {
                if (child->type != Type::WILDCARD) {
                    continue;
                }
                remaining = {};
                node = child.get();
                matched = true;
                break;
            }

            if (!matched) {
                return nullptr;
            }
        }

        return remaining.empty() ? node : nullptr;
    }

private:
    static size_t common_prefix(std::string_view a, std::string_view b) {
        size_t i = 0;
        size_t len = std::min(a.size(), b.size());
        while (i < len && a[i] == b[i]) ++i;
        return i;
    }
};

} // namespace async_uv::layer3
