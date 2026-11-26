target "aarch64-apple-darwin"

type @S = { i64, i64 }

extern func @take_s(byval(@S)) -> i64 cc "c"

func @bad_call() -> i64 {
.entry:
    %r = call @take_s(0)
    ret %r
}
