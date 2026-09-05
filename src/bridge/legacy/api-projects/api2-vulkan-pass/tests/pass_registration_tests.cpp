#include <sa_api2/pass_registration.hpp>
#include <cassert>
int main(){ assert(!sa::api2::PassRegistration::create(nullptr,{},0,nullptr,nullptr).has_value()); return 0; }
