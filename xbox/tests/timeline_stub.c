/* Timeline is a host diagnostic; unit tests that compile d3d8_hle.c alone
 * do not link the executable-only timeline implementation. */
unsigned recomp_timeline_swap_begin(void) { return 0; }
void recomp_timeline_swap_end(void) {}
unsigned recomp_timeline_frame(void) { return 0; }
void recomp_timeline_event(const char *kind, const char *detail,
                           double value0, double value1)
{
    (void)kind; (void)detail; (void)value0; (void)value1;
}

int recomp_call_profile_on = 0;
void recomp_call_profile_enter(unsigned int va) { (void)va; }
void recomp_call_profile_exit(unsigned int va) { (void)va; }
void recomp_guest_profile_init(void) {}
void recomp_guest_profile_update_window(unsigned long flip) { (void)flip; }
int recomp_guest_profile_stack(unsigned int *out, int capacity) { (void)out; (void)capacity; return 0; }

int recomp_count_on = 0;
void recomp_region_count(unsigned id) { (void)id; }
void recomp_region_enter(unsigned id) { (void)id; }
void recomp_region_exit(unsigned id) { (void)id; }
void recomp_count_init(void) {}
void recomp_count_update_window(unsigned long flip) { (void)flip; }
int recomp_pair_time_enabled(void) { return 0; }
void recomp_pair_time_init(void) {}
void recomp_pair_time_update_window(unsigned long flip) { (void)flip; }
void recomp_pt_enter(unsigned id) { (void)id; }
void recomp_pt_exit(unsigned id) { (void)id; }
int recomp_render_capture_enabled(void) { return 0; }
void recomp_render_capture_init(void) {}
void recomp_render_capture_update_window(unsigned long flip) { (void)flip; }
void recomp_rcap_snapshot(const char *type, unsigned addr, unsigned size,
                          unsigned a0, unsigned a1, unsigned a2)
{ (void)type; (void)addr; (void)size; (void)a0; (void)a1; (void)a2; }
void recomp_rcap_state(const char *type, unsigned a0, unsigned a1,
                       unsigned a2)
{ (void)type; (void)a0; (void)a1; (void)a2; }
void recomp_rcap_sync(const char *name) { (void)name; }
void recomp_rcap_unsupported(const char *name) { (void)name; }
void recomp_rcap_require(const char *type, unsigned size)
{ (void)type; (void)size; }
void recomp_rcap_measure(const char *type, unsigned addr, unsigned size)
{ (void)type; (void)addr; (void)size; }
void recomp_rcap_manifest(const char *type, unsigned addr, unsigned total,
                          unsigned valid, const unsigned *fields,
                          unsigned field_count)
{ (void)type; (void)addr; (void)total; (void)valid; (void)fields;
  (void)field_count; }
void recomp_rcap_texture_bind(unsigned stage, unsigned resource)
{ (void)stage; (void)resource; }
int recomp_rcap_active(void) { return 0; }
void recomp_rcap_renderer_bytes(unsigned long long content, unsigned long long upload)
{ (void)content; (void)upload; }
void recomp_rcap_texture_frame(unsigned frame) { (void)frame; }
