#include <random>
#include <mongocxx/client.hpp>
#include <memory>
#include <unordered_map>

#pragma once
using namespace std;

namespace mwg {

class node;

class threadState {
public:
    threadState(){};
    threadState(uint64_t seed,
                unordered_map<string, int64_t> tvars,
                unordered_map<string, int64_t>* wvars)
        : rng(seed), tvariables(tvars), wvariables(wvars){};
    mongocxx::client conn{};
    mt19937_64 rng;  // random number generator
    shared_ptr<node> currentNode;
    unordered_map<string, int64_t> tvariables;
    unordered_map<string, int64_t>* wvariables;  // FIXME: Not threadsafe
};
}
