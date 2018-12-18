#include <gennylib/PoolFactory.hpp>

#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include <boost/log/trivial.hpp>
#include <mongocxx/uri.hpp>

#include <gennylib/InvalidConfigurationException.hpp>

namespace genny {

struct PoolFactory::Config {
    Config(std::string_view uri) {
        const auto protocolRegex = std::regex("^(mongodb://|mongodb+srv://)?(([^:@]*):([^@]*)@)?");
        const auto hostRegex = std::regex("^,?([^:,/]+(:[0-9]+)?)");
        const auto dbRegex = std::regex("^/([^?]*)\\??");
        const auto queryRegex = std::regex("^&?([^=&]*)=([^&]*)");

        std::cmatch matches;
        auto i = 0;

        // Extract the protocol, and optionally the username and the password
        std::regex_search(uri.substr(i).data(), matches, protocolRegex);
        if (matches.length(1)) {
            accessOptions["Protocol"] = matches[1];
        } else {
            accessOptions["Protocol"] = "mongodb://";
        }
        if (matches.length(3))
            accessOptions["Username"] = matches[3];
        if (matches.length(4))
            accessOptions["Password"] = matches[4];
        i += matches.length();

        // Extract each host specified in the uri
        while (std::regex_search(uri.substr(i).data(), matches, hostRegex)) {
            hosts.insert(matches[1]);
            i += matches.length();
        }

        // Extract the db name and optionally the query string prefix
        std::regex_search(uri.substr(i).data(), matches, dbRegex);
        accessOptions["Database"] = matches[1];
        i += matches.length();

        // Extract each query parameter
        // Note that the official syntax of query strings is poorly defined, keys without values may
        // be valid but not supported here.
        while (std::regex_search(uri.substr(i).data(), matches, queryRegex)) {
            auto key = matches[1];
            auto value = matches[2];
            queryOptions[key] = value;
            i += matches.length();
        }
    }

    auto makeUri() const {
        std::ostringstream ss;
        size_t i;

        ss << accessOptions.at("Protocol");
        if (!accessOptions.at("Username").empty()) {
            ss << accessOptions.at("Username") << ':' << accessOptions.at("Password") << '@';
        }

        i = 0;
        for (auto& host : hosts) {
            if (i++ > 0) {
                ss << ',';
            }
            ss << host;
        }

        auto dbName = accessOptions.at("Database");
        if (!dbName.empty() || !queryOptions.empty()) {
            ss << '/' << accessOptions.at("Database");
        }

        if (!queryOptions.empty()) {
            ss << '?';
        }

        i = 0;
        for (auto&& [key, value] : queryOptions) {
            if (i++ > 0) {
                ss << '&';
            }
            ss << key << '=' << value;
        }

        return ss.str();
    }

    void set(OptionType type, const std::string& key, std::string value) {
        switch (type) {
            case OptionType::kQueryOption: {
                queryOptions[key] = value;
                return;
            }
            case OptionType::kAccessOption: {
                // Access options are a predefined set
                auto it = accessOptions.find(key);
                if (it == accessOptions.end()) {
                    std::ostringstream ss;
                    ss << "Attempted to set unknown access option '" << key << "'";
                    throw InvalidConfigurationException(ss.str());
                }
                it->second = value;
                return;
            }
        }

        throw InvalidConfigurationException("Did not recognize OptionType in setter");
    }

    std::optional<std::string_view> get(OptionType type, const std::string& key) {
        switch (type) {
            case OptionType::kQueryOption: {
                auto it = queryOptions.find(key);
                if (it == queryOptions.end())
                    return std::nullopt;

                return std::make_optional<std::string_view>(it->second);
            }
            case OptionType::kAccessOption: {
                // Access options are a predefined set
                auto it = accessOptions.find(key);
                if (it == accessOptions.end()) {
                    std::ostringstream ss;
                    ss << "Attempted to get unknown access option '" << key << "'";
                    throw InvalidConfigurationException(ss.str());
                }
                return std::make_optional<std::string_view>(it->second);
            }
        }

        throw InvalidConfigurationException("Did not recognize OptionType in getter");
    }

    bool getFlag(OptionType type, const std::string& key, bool defaultValue = false) {
        auto opt = get(type, key);
        if (!opt)
            return defaultValue;

        return *opt == "true";
    }

    std::set<std::string> hosts;
    std::map<std::string, std::string> queryOptions;
    std::map<std::string, std::string> accessOptions = {
        {"Protocol", ""},
        {"Username", ""},
        {"Password", ""},
        {"Database", ""},
        {"AllowInvalidCertificates", ""},
        {"CAFile", ""},
        {"PEMKeyFile", ""},
    };
};

PoolFactory::PoolFactory(std::string_view rawUri) : _config(std::make_unique<Config>(rawUri)) {}
PoolFactory::~PoolFactory() {}

std::string PoolFactory::makeUri() const {
    return _config->makeUri();
}

mongocxx::options::pool PoolFactory::makeOptions() const {
    mongocxx::options::ssl sslOptions;

    auto allowInv = _config->getFlag(OptionType::kAccessOption, "AllowInvalidCertificates");
    if (allowInv) {
        BOOST_LOG_TRIVIAL(debug) << "Allowing invalid certificates for SSL/TLS";
        sslOptions = sslOptions.allow_invalid_certificates(true);
    }

    // Just doing CAFile and PEMKeyFile for now, it's reasonably trivial to add other options
    // Note that this is entering as a BSON string view, so you cannot delete the config object
    auto caFile = *_config->get(OptionType::kAccessOption, "CAFile");
    if (!caFile.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "Using CA file '" << caFile << "' for SSL/TLS";
        sslOptions = sslOptions.ca_file(caFile.data());
    }

    auto pemKeyFile = *_config->get(OptionType::kAccessOption, "PEMKeyFile");
    if (!pemKeyFile.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "Using PEM Key file '" << pemKeyFile << "' for SSL/TLS";
        sslOptions = sslOptions.pem_file(pemKeyFile.data());
    }

    return mongocxx::options::client{}.ssl_opts(sslOptions);
}
std::unique_ptr<mongocxx::pool> PoolFactory::makePool() const {
    auto uriStr = makeUri();
    BOOST_LOG_TRIVIAL(info) << "Constructing pool with MongoURI '" << uriStr << "'";

    auto uri = mongocxx::uri{uriStr};

    auto poolOptions = mongocxx::options::pool{};
    auto useSsl = _config->getFlag(OptionType::kQueryOption, "ssl");
    if (useSsl) {
        poolOptions = makeOptions();
        BOOST_LOG_TRIVIAL(debug) << "Adding ssl options to pool...";
    }

    return std::make_unique<mongocxx::pool>(uri, poolOptions);
}

void PoolFactory::setOption(OptionType type, const std::string& option, std::string value) {
    _config->set(type, option, value);
}

void PoolFactory::setOptionFromInt(OptionType type, const std::string& option, int32_t value) {
    auto valueStr = std::to_string(value);
    setOption(type, option, valueStr);
}

void PoolFactory::setFlag(OptionType type, const std::string& option, bool value) {
    auto valueStr = value ? "true" : "false";
    setOption(type, option, valueStr);
}

std::optional<std::string_view> PoolFactory::getOption(OptionType type,
                                                       const std::string& option) const {
    return _config->get(type, option);
}

}  // namespace genny