cc_binary(
    name = "envoy_esp",
    srcs = [
        "api_manager_filter.cc",
        "api_manager_env.cc",
        "api_manager_env.h",
    ],
    visibility = [":__subpackages__"],
    deps = [
        "//external:protobuf",
        "//include:api_manager",
        "//src/api_manager/grpc:zero_copy_stream",
        "//external:servicecontrol",
        "//external:grpc",
        "@envoy_git//:envoy-common"
    ],
    linkstatic=1,
)
