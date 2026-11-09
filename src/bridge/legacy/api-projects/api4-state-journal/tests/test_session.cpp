#include "api4_journal_session.hpp"
#include "../../tests/test_check.hpp"
int main() {
  CHECK(!api4::JournalSession::begin(nullptr).has_value());
  return api_test::finish();
}
