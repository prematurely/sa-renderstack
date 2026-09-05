#include "api7_selective_journal.hpp"
#include <cassert>

int main() {
  api7::SelectiveJournal journal(nullptr);
  assert(!journal.begin().has_value());
  assert(!journal.capture(true).has_value());
  assert(!journal.restore().has_value());
}
