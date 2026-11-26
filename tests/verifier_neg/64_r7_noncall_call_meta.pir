target "aarch64-apple-darwin"

type @S = { i64 }

func @bad_meta(%p: ptr) -> i64 {
.entry:
    %v = add.i64 1, 2: byval(@S) [wrap]
    ret %v
}
