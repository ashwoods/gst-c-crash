# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]
### Changed
- Faulty tests run each method in a gst-launch-1.0 subprocess and check its exit signal or error, so a crash no longer kills pytest. The `custom` and `stop` methods are marked as known failures.

### Fixed
- Faulty's `custom` method now crashes with SIGSEGV: methods map to their crash functions explicitly instead of indexing a table by the enum value, and the null-pointer write is volatile so the compiler cannot delete it.