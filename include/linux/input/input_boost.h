#ifdef CONFIG_CPU_FREQ_LIMITS_ON_SUSPEND
extern u64 last_input_time;
extern unsigned int input_boost_ms;
extern unsigned int input_boost_freq;
#else
static u64 last_input_time = 0;
static unsigned int input_boost_ms = 0;
static unsigned int input_boost_freq = 0;
#endif