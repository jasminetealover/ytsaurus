GO_LIBRARY()

LICENSE(Apache-2.0)

VERSION(v1.2.2)

SRCS(
    stdr.go
)

GO_XTEST_SRCS(example_test.go)

END()

RECURSE(
    gotest
)
