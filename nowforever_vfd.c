/*
    nowforever_vfd.c

    This is a userspace program that intefaces the Nowforever D100/E100 VFD
    to the LinuxCNC HAL, using RS485 ModBus RTU.
*/

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>

#include <modbus.h>

#include "hal.h"
#include "rtapi.h"


// If a modbus transaction fails, retry this many times before giving up.
#define NUM_MODBUS_RETRIES 5

#define START_REGISTER_R        0x0500
#define NUM_REGISTER_R          8
#define VFD_INSTRUCTION         0x0900  // Bit 0: 1 = run, 0 = stop
                                        // Bit 1: 1 = reverse, 0 = forward
                                        // Bit 2: 1 = JOG, 0 = stop JOG
                                        // Bit 3: 1 = fault reset 0 = no reset
#define VFD_FREQUENCY           0x0901  // Write frequency in 0.01 Hz steps
#define MAX_RPM                 24000
#define MAX_FREQ                400

typedef struct {
    int slave;
    int read_reg_start;
    int read_reg_count;
} slavedata_t;

slavedata_t slavedata;

typedef struct {
    // Read pin from VFD, (inverter running state)
    hal_s32_t       *inverter_status;
    hal_float_t     *freq_cmd;
    hal_float_t     *output_freq;
    hal_float_t     *output_current;
    hal_float_t     *output_volt;
    hal_s32_t       *dc_bus_volt;
    hal_float_t     *motor_load;
    hal_s32_t       *inverter_temp;
    hal_bit_t       *vfd_error;

    // Created
    hal_bit_t       *at_speed;
    hal_bit_t       *is_stopped;
    hal_float_t     *output_rpm;

    // In to VFD
    hal_bit_t       *spindle_on;
    hal_bit_t       *spindle_fwd;
    hal_bit_t       *spindle_rev;
    hal_float_t     *rpm_cmd;

    // Parameter
    hal_float_t     speed_tolerance;
    hal_float_t     period;
    hal_s32_t       modbus_errors;
    hal_s32_t       retval;
} haldata_t;

haldata_t *haldata;


int hal_comp_id;

static int done;
char *modname = "nowforever_vfd";

int read_data(modbus_t *mb_ctx, slavedata_t *slavedata, haldata_t *hal_data_block) {
    uint16_t receive_data[MODBUS_MAX_READ_REGISTERS];
    int retval;

    // Can't do anything with an empty datablock
    if (hal_data_block == NULL)
        return -1;

    // Signal error if parameter is null    
    if ((mb_ctx == NULL) || (slavedata == NULL))
    {
        hal_data_block -> modbus_errors++;
        return -1;
    }

    retval = modbus_read_registers(mb_ctx, slavedata -> read_reg_start,
                                slavedata -> read_reg_count, receive_data);
    
    if (retval == slavedata -> read_reg_count) {
        retval = 0;
        hal_data_block -> retval = retval;
        if (retval == 0) {
            *(hal_data_block -> inverter_status) = receive_data[0];
            *(hal_data_block -> freq_cmd) = receive_data[1] * 0.01;
            *(hal_data_block -> output_freq) = receive_data[2] * 0.01;
            *(hal_data_block -> output_current) = receive_data[3] * 0.1;
            *(hal_data_block -> output_volt) = receive_data[4] * 0.1;
            *(hal_data_block -> dc_bus_volt) = receive_data[5];
            *(hal_data_block -> motor_load) = receive_data[6] * 0.1;
            *(hal_data_block -> inverter_temp) = receive_data[7];
        }
    } else {
        hal_data_block -> retval = retval;
        hal_data_block -> modbus_errors++;
        retval = -1;
    }    
    return retval;
}

int set_motor(modbus_t *mb_ctx, haldata_t *haldata) {
    uint16_t val;

    // Run cw
    if (*haldata -> spindle_on && *haldata -> spindle_fwd &&
    (*haldata -> inverter_status & 3) != 1) {
        val = 1;
    }
    // Run ccw
    else if (*haldata -> spindle_on != *haldata -> spindle_fwd &&
    (*haldata -> inverter_status & 3) != 3) {
        val = 3;
    } 
    // Stop
    else if (!*haldata -> spindle_on && (*haldata -> inverter_status & 1) != 0) {
        val = 0;
    } else {
        return 0;
    }

    for (int retries = 0; retries <= NUM_MODBUS_RETRIES; retries++) {
        if (modbus_write_registers(mb_ctx, VFD_INSTRUCTION, 0x01, &val) == 1) {
            return 0;
        }
        fprintf(stderr, "%s: error writing %u to register 0x04%x: %s\n", __func__, val,
                        VFD_INSTRUCTION, modbus_strerror(errno));
        haldata -> modbus_errors++;
    }
    return -1;
}

int set_motor_frequency(modbus_t *mb_ctx, haldata_t *haldata, float freq) {
    // Modbus cant handle floats
    uint16_t val;
    val = freq * 100;
     
   // Cap at max frequency
    if (val > MAX_FREQ * 100) {
        val = MAX_FREQ * 100;
    }

    for (int retries = 0; retries <= NUM_MODBUS_RETRIES; retries++) {
        if (modbus_write_registers(mb_ctx, VFD_FREQUENCY, 0x01, &val) == 1) {
            return 0;
        }
        fprintf(stderr, "%s: error writing %u to register 0x%04x: %s\n", __func__, val,
                        VFD_FREQUENCY, modbus_strerror(errno));
        haldata -> modbus_errors++;
    }
    return -1;
}

// Wrapper function to write to vfd and set HAL pins
void write_data(modbus_t *mb_ctx, haldata_t *haldata) {
    set_motor(mb_ctx, haldata);

    // The vfd dosen't like negative numbers
    if (*haldata -> rpm_cmd < 0) {
        *haldata -> rpm_cmd = fabsf(*haldata -> rpm_cmd);
    }

    // Calculate frequency with 2 decimals
    int freq;
    freq = (int)(((*haldata -> rpm_cmd / MAX_RPM) * MAX_FREQ) * 100);
    *haldata -> freq_cmd = (float) freq / 100;

    // If equal, we don't write to vfd.
    if (*haldata -> freq_cmd != *haldata -> output_freq) {
        set_motor_frequency(mb_ctx, haldata, *haldata -> freq_cmd);
    }

    if (*haldata -> output_freq == 0) {
        *haldata -> is_stopped = 1;
    } else {
        *haldata -> is_stopped = 0;
    }

    *haldata -> output_rpm = *haldata -> output_freq * (MAX_RPM / MAX_FREQ);
    
    // Calculates in % difference between set and actual frequency
    if (fabsf(1 - (*haldata -> freq_cmd / *haldata -> output_freq)) < haldata -> speed_tolerance) {
        *(haldata -> at_speed) = 1;
    } else {
        *haldata -> at_speed = 0;
    }
    
    if (*haldata -> spindle_on == 0) {
        *(haldata -> at_speed) = 0;
    }

    if ((*haldata -> inverter_status & 24) != 0) {
        *haldata -> vfd_error = 1;
    }
}

// Command-line options
static struct option long_options[] = {
    {"device", 1, 0, 'd'},
    {"name", 1, 0 , 'n'},
    {"parity", 1, 0, 'p'},
    {"rate", 1, 0, 'r'},
    {"verbose", 0, 0, 'v'},
    {"target", 1, 0, 't'},
    {"help", 0, 0, 'h'},
    {0,0,0,0}
};

static char *option_string = "d:n:p:r:vt:h";

static char *paritystrings[] = {"even", "odd", "none", NULL};
static char paritychars[] = {'E', 'O', 'N'};

static char *ratestrings[] = {"2400", "4800", "9600", "19200", "38400", NULL};

static void quit(int sig) {
    done = 1;
}

int match_string(char *string, char **matches) {
    int len, which, match;
    which = 0;
    match = -1;
    if ((matches == NULL) || (string == NULL)) return -1;
    len = strlen(string);
    while (matches[which] != NULL) {
        if ((!strncmp(string, matches[which], len)) && (len <= strlen(matches[which]))) {
            if (match >= 0) return -1;  // Multiple matches
            match = which;
        }
        ++which;
    }
    return match;
}

void usage(int argc, char **argv) {
    printf("Usage: %s [ARGUMENTS]\n", argv[0]);
    printf(
    "\n"
    "This program interfaces the Nowforever D100/E100 VFD to the LinuxCNC HAL.\n"
    "\n"
    "Optional arguments:\n"
    "   -d, -device <path> (default: /dev/ttyUSB0)\n"
    "       Set the name of the serial device to use\n"
    "   -n, --name <string> (default: nowforever_vfd)\n"
    "       Set the name of the HAL module.  The HAL comp name will be set to <string>, and all pin\n"
    "       and parameter names will begin with <string>.\n"
    "   -p, --parity {even,odd,none} (default: none)\n"
    "       Set serial parity to 'even', 'odd', or 'none'.\n"
    "   -r, --rate <n> (default: 19200)\n"
    "       Set baud rate to <n>. It is an error if the rate is not one of the following:\n"
    "       2400, 4800, 9600, 19200, 38400\n"
    "   -t, --target <n> (default: 1)\n"
    "       Set Modbus target (slave) number. This must match the device\n"
    "       number you set on the Nowforever VFD.\n"
    "   -v, --verbose\n"
    "       Turn on verbose mode.\n"
    "   -h, --help\n"
    "       Show this help.\n"
    );
}

int main(int argc, char **argv) {
    char *device;
    int baud;
    int bits;
    char parity;
    int stopbits;
    int verbose;

    int retval = 0;
    modbus_t *mb_ctx;
    int slave;
    struct timespec period_timespec;
    char *endarg;
    int opt;
    int argindex, argvalue;

    done = 0;

    // Assume that nothing is specified on the command line
    device = "/dev/ttyUSB0";
    baud = 19200;
    bits = 8;
    parity = 'N';
    stopbits = 1;

    verbose = 0;

    slave = 1;
    slavedata.read_reg_start = START_REGISTER_R;
    slavedata.read_reg_count = NUM_REGISTER_R;

    // Process command line options
    while ((opt = getopt_long(argc, argv, option_string, long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':  // Device name, default /dev/ttyUSB0
                // Could check the device name here, but we'll leave it to the library open
                if (strlen(optarg) > FILENAME_MAX) {
                    printf("ERROR: device node name is to long: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                device = strdup(optarg);
                break;

            // Module base name
            case 'n':  
                if (strlen(optarg) > HAL_NAME_LEN - 20) {
                    printf("ERROR: HAL module name to long: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                modname = strdup(optarg);
                break;

            case 'p':  // Parity, should be a string like "even", "odd" or "none"
                argindex = match_string(optarg, paritystrings);
                if (argindex < 0) {
                    printf("ERROR: invalid parity: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                parity = paritychars[argindex];
                break;

            case 'r':  // Baud rate, defaults to 19200
                argindex = match_string(optarg, ratestrings);
                if (argindex < 0) {
                    printf("ERROR: invalid baud rate: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                baud = atoi(ratestrings[argindex]);
                break;
            
            case 't':  // Target number (MODBUS ID), default 1
                argvalue = strtol(optarg, &endarg, 10);
                if ((*endarg != '\0') || (argvalue < 1) || (argvalue > 31)) {
                    printf("ERROR: invalid slave number: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                slave = argvalue;
                break;

            case 'v':
                verbose = 1;
                break;

            case 'h':
                usage(argc, argv);
                exit(0);
                break;

            default:
                usage(argc, argv);
                exit(1);
                break;
        }
    }

    printf("%s: device='%s', baud='%d', bits=%d, parity='%c', stopbits=%d, address=%d\n",
            modname, device, baud, bits, parity, stopbits, slave);

    // Point TERM and INT signals at our quit function.
    // If a signal is received between here and the main loop, it should prevent
    // some initialization from happening.
    signal(SIGINT, quit);
    signal(SIGTERM, quit);

    // Assume 19200 bps 8-N-1 serial setting, device 1
    mb_ctx = modbus_new_rtu(device, baud, parity, bits, stopbits);
    if (mb_ctx == NULL) {
        printf("%s: ERROR: Couldn't open modbus serial device: %s\n", modname, modbus_strerror(errno));
        goto out_noclose;
    }

    retval = modbus_connect(mb_ctx);
    if (retval != 0) {
        printf("%s: ERROR: Couldn't open serial device: %s\n", modname, modbus_strerror(errno));
        goto out_noclose;
    }

    modbus_set_debug(mb_ctx, verbose);

    modbus_set_slave(mb_ctx, slave);

    // Create HAL component
    hal_comp_id = hal_init(modname);
    if (hal_comp_id < 0) {
        printf("%s: ERROR: hal_init failed\n", modname);
        retval = hal_comp_id;
        goto out_close;
    }

    haldata = (haldata_t *)hal_malloc(sizeof(haldata_t));
    if (haldata == NULL) {
        printf("%s: ERROR: unable to allocate shared memory\n", modname);
        retval = -1;
        goto out_closeHAL;
    }

    retval = hal_pin_s32_newf(HAL_OUT, &(haldata -> inverter_status), hal_comp_id, "%s.inverter-status", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_OUT, &(haldata -> freq_cmd), hal_comp_id, "%s.frequency-command", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_OUT, &(haldata -> output_freq), hal_comp_id, "%s.frequecy-out", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_OUT, &(haldata -> output_current), hal_comp_id, "%s.output-current", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_OUT, &(haldata -> output_volt), hal_comp_id, "%s.output-volt", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_s32_newf(HAL_OUT, &(haldata -> dc_bus_volt), hal_comp_id, "%s.DC-bus-volt", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_OUT, &(haldata -> motor_load), hal_comp_id, "%s.load-percentage", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_s32_newf(HAL_OUT, &(haldata -> inverter_temp), hal_comp_id, "%s.inverter-temp", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_bit_newf(HAL_OUT, &(haldata -> vfd_error), hal_comp_id, "%s.vfd-error", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_bit_newf(HAL_OUT, &(haldata -> at_speed), hal_comp_id, "%s.at-speed", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_bit_newf(HAL_OUT, &(haldata -> is_stopped), hal_comp_id, "%s.is-stopped", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_OUT, &(haldata -> output_rpm), hal_comp_id, "%s.motor-RPM", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_bit_newf(HAL_IN, &(haldata -> spindle_on), hal_comp_id, "%s.spindle-on", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_bit_newf(HAL_IN, &(haldata -> spindle_fwd), hal_comp_id, "%s.spindle-fwd", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_bit_newf(HAL_IN, &(haldata -> spindle_rev), hal_comp_id, "%s.spindle-rev", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_pin_float_newf(HAL_IN, &(haldata -> rpm_cmd), hal_comp_id, "%s.speed-command", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_param_float_newf(HAL_RW, &(haldata -> speed_tolerance), hal_comp_id, "%s.tolerance", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_param_float_newf(HAL_RW, &(haldata -> period), hal_comp_id, "%s.period-seconds", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_param_s32_newf(HAL_RO, &(haldata -> modbus_errors), hal_comp_id, "%s.modbus-errors", modname);
    if (retval != 0) goto out_closeHAL;

    retval = hal_param_s32_newf(HAL_RO, &(haldata -> retval), hal_comp_id, "%s.retval", modname);
    if (retval != 0) goto out_closeHAL;
    
    // Make default data match what we expect to use
    *haldata -> inverter_status = 0;
    *haldata -> freq_cmd = 0.0;
    *haldata -> output_freq = 0.0;
    *haldata -> output_current = 0.0;
    *haldata -> output_volt = 0.0;
    *haldata -> dc_bus_volt = 0;
    *haldata -> motor_load = 0.0;
    *haldata -> inverter_temp = 0;
    *haldata -> vfd_error = 0;

    *haldata -> at_speed = 0;
    *haldata -> is_stopped = 0;
    *haldata -> rpm_cmd = 0;

    haldata -> speed_tolerance = 0.01;
    haldata -> period = 0.1;
    haldata -> modbus_errors = 0;

    // Activate HAL component
    hal_ready(hal_comp_id);

    while (done == 0) {
        // Don't scan to fast, and not delay more than a few seconds
        if (haldata -> period < 0.001) haldata -> period = 0.001;
        if (haldata -> period > 2.0) haldata -> period = 2.0;
        period_timespec.tv_sec = (time_t)(haldata -> period);
        period_timespec.tv_nsec = (long)((haldata -> period - period_timespec.tv_sec) * 1000000000l);
        nanosleep(&period_timespec, NULL);

        read_data(mb_ctx, &slavedata, haldata);
        write_data(mb_ctx, haldata);
    }

    // If we get here, then everythin is fine, so just clean up and exit
    retval = 0;
out_closeHAL:
    hal_exit(hal_comp_id);
out_close:
    modbus_close(mb_ctx);
    modbus_free(mb_ctx);
out_noclose:
    return retval;
}