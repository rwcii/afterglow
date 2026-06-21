# radio_backend — pure interface (HAL vtable + types). Header-only; the concrete
# backend (radio_single) implements the vtable and provides radio_backend_get().
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES afterglow_core   # ag_core/ag_eligible.h (ag_adv_kind_t) in a public header
)
