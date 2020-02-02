set(COMPONENT_SRCS "tractor.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")

register_component()

target_add_binary_data(${COMPONENT_TARGET} "certs/private_key.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/google_roots.pem" TEXT)

