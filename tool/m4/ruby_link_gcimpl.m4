dnl -*- Autoconf -*-
AC_DEFUN([RUBY_GCIMPL],[
        AS_CASE(["$target_os"],
                [darwin*], [RUBY_GC_SONAME=libgcimpl.dylib],
                [
                        RUBY_GC_SONAME=libgcimpl.so
                ])
])dnl
