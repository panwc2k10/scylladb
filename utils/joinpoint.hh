/*
 * Copyright (C) 2016-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <memory>
#include <optional>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>

#include "seastarx.hh"

/**
 * Joinpoint:
 *
 * Helper type for letting operations working on all shards "join" and acquire
 * the same value of something, with that value being based on whenever that
 * join takes place. (Obvious use case: time stamp after one set of per-shard ops, but
 * before final ones).
 * The generation of the value is guaranteed to happen on the shards that created the
 * join point.
 */
namespace utils {

template<typename T>
class joinpoint {
public:
    typedef typename futurize<T>::type type;
    typedef std::function<type()> func_type;

    joinpoint(func_type f)
        : _func(std::move(f))
        , _shard(this_shard_id())
        , _enter(0)
        , _wait(0)
    {}
    type value() {
        return smp::submit_to(_shard, [this, id = this_shard_id()] {
            _enter.signal();
            if (id == _shard) {
                // We should not generate to common value until all shards
                // have reached this point. Thus the two semaphores.
                return _enter.wait(smp::count).then([this] {
                    return _func().then([this](T v) {
                        _value = std::move(v);
                        _wait.signal(smp::count - 1); // we don't wait
                        return make_ready_future<T>(*_value);
                    }).handle_exception([this](auto ep) {
                        _wait.broken(ep);
                        return make_exception_future<T>(ep);
                    });
                });
            }
            return _wait.wait().then([this] {
                assert(_value);
                return make_ready_future<T>(*_value);
            });
        });
    }
private:
    func_type _func;
    shard_id _shard;
    semaphore _enter;
    semaphore _wait;
    std::optional<T> _value;
};

/**
 * Based on the join-code in cf::snapshot.
 * An object that allows us to generate a value for-all-shards
 * at some point down the execution in multiple shards.
 *
 * T type must be copyable, and preferable primitive/trivial
 * or at the very least shard-copy safe.
 */
template<typename Func, typename T = std::result_of_t<Func()>>
joinpoint<T> make_joinpoint(Func && f) {
    return joinpoint<T>([f = std::forward<Func>(f)] {
        return futurize_invoke(f);
    });
}

}
