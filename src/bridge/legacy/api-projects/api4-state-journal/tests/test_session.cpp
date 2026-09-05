#include "api4_journal_session.hpp"
#include <cassert>
int main(){ assert(!api4::JournalSession::begin(nullptr).has_value()); return 0; }
