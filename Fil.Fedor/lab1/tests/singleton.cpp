#include "daemon.hpp"

#include <cassert>
#include <type_traits>

static_assert(!std::is_default_constructible_v<Daemon>);
static_assert(!std::is_copy_constructible_v<Daemon>);
static_assert(!std::is_copy_assignable_v<Daemon>);

int main() {
    assert(&Daemon::instance() == &Daemon::instance());
}
