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
        "//external:servicecontrol",
        "@envoy_git//:envoy-common"
    ],
    linkstatic=1,
)
