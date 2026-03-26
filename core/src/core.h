#pragma once

class konacore {
public:
  konacore(const konacore &) = default;
  konacore(konacore &&) = default;
  konacore &operator=(const konacore &) = default;
  konacore &operator=(konacore &&) = default;
  constexpr static const char *project = "KonataDancer";
  constexpr static short version[3] = {2, 0, 0};
};