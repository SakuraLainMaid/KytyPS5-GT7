# Profiles for the optimized Windows build

The GitHub Windows build compiles the emulator with the newest `.profdata` file in this folder
(by name), when its clang-cl can read it (profile-guided optimization: the compiler lays out and
inlines the code by how a recorded run used it). Without a readable profile it builds as before.

A profile is made from a recorded run with the training build (`KYTY_PGO_GENERATE=ON`, run with
`KYTY_PGO_FILE=<file.profraw>`), then merged with the `llvm-profdata` of the same clang version:

```
llvm-profdata merge -o race-rNNN.profdata <file.profraw>
```

Name new profiles so they sort after the old ones (`race-r150.profdata`, `race-r170.profdata`, ...).
Code that changed after the recorded run is compiled without the profile's guidance, so a profile
is worth making again now and then. Function names in a profile carry only a source file's name,
not its folder, so a profile recorded in one checkout serves another.
