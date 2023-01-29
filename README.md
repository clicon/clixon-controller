<div align="center">
  <img src="https://www.clicon.org/Clixon_logga_liggande_med-ikon.png" width="400">
</div>

[![Build Status](https://github.com/clicon/clixon-controller/actions/workflows/test.yml/badge.svg)](https://github.com/clicon/clixon-controller/actions/workflows/test.yml)

# clixon-controller
Clixon network controller

## Build clixon

See https://clixon-docs.readthedocs.io/en/latest/install.html

Change as follows before compile:
```
diff --git a/include/clixon_custom.h b/include/clixon_custom.h
index ce4e5f27..c49c4c0b 100644
--- a/include/clixon_custom.h
+++ b/include/clixon_custom.h
@@ -196,4 +196,4 @@
  * Experimental
  * See also test/test_yang_schema_mount.sh
  */
-#undef YANG_SCHEMA_MOUNT
+#define YANG_SCHEMA_MOUNT
```

## Build and install:

```
> ./configure
> make        # native build
> sudo make install

```
