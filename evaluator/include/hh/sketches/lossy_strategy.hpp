#pragma once
#ifndef LOSSYSTRATEGY_H
#define LOSSYSTRATEGY_H

#include <stdint.h>
#include <string>
#include <cmath>
#include <cstdlib>
#include <cstdint>

namespace Lossy {

class BaseStrategy {
public:
    virtual ~BaseStrategy() = default;  // <-- add this line
    virtual void AlgorithmInterface(int32_t &num, std::string &oldstr,
                                    std::string newstr, std::uint32_t random_value) = 0;
};

// 0
class MinusOneStrategy : public BaseStrategy {
public:
    void AlgorithmInterface(int32_t &num, std::string &oldstr,
                            std::string newstr, std::uint32_t) override {
        if (num > 0) { --num; }
        if (num <= 0) {
            oldstr = newstr;
            num = 1;
        }
    }
};

// 1
class HeavyKeeperStrategy : public BaseStrategy {
public:
    void AlgorithmInterface(int32_t &num, std::string &oldstr,
                            std::string newstr, std::uint32_t random_value) override {
        if (!(random_value % int(std::pow(1.08, num)))) { // heavykeeper
            num--;
        }
        if (num <= 0) {
            oldstr = newstr;
            num = 1;
        }
    }
};

// 2
class RAPStrategy : public BaseStrategy {
public:
    void AlgorithmInterface(int32_t &num, std::string &oldstr,
                            std::string newstr, std::uint32_t random_value) override {
        if (!(random_value % (num + 1))) {             // RAP replacement
            oldstr = newstr;
            num++;
        }
    }
};

// 3
class USSStrategy : public BaseStrategy {
public:
    void AlgorithmInterface(int32_t &num, std::string &oldstr,
                            std::string newstr, std::uint32_t random_value) override {
        num++;                                        // USS replacement
        if (!(random_value % (num))) {
            oldstr = newstr;
        }
    }
};

class Context {
public:
    BaseStrategy *strategy;
    explicit Context(int AlgorithmMode) {
        switch (AlgorithmMode) {
        case 0: strategy = new MinusOneStrategy(); break;
        case 1: strategy = new HeavyKeeperStrategy(); break;
        case 2: strategy = new RAPStrategy(); break;
        case 3: strategy = new USSStrategy(); break;
        default: strategy = nullptr; break;
        }
    }
    void ContextInterface(int32_t &num, std::string &oldstr, std::string newstr,
                          std::uint32_t random_value) {
        this->strategy->AlgorithmInterface(num, oldstr, newstr, random_value);
    }
    ~Context() { delete strategy; } // ok now that BaseStrategy has a virtual dtor
};

} // namespace Lossy

#endif // LOSSYSTRATEGY_H
