/**
 * @file nowforever_vfd.c
 * @brief A userspace program that interfaces the Nowforever D100/E100 VFD
 *        to the LinuxCNC HAL, using RS485 ModBus RTU.
 */

/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2020-2023 Håvard F. Aasen <havard.f.aasen@pfft.no>
 *
 * Based on other drivers found in the LinuxCNC repository.
 */

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <modbus.h>

#include "hal.h"
#include "rtapi.h"


/** If a modbus transaction fails, retry this many times before giving up. */
#define NUM_MODBUS_RETRIES 5

/** Address of register to read from. */
#define START_REGISTER_READ     0x0500

/** Number of registers to read */
#define NUM_REGISTER_READ       8

/**
 * Bit 0: 1 = run, 0 = stop @n
 * Bit 1: 1 = reverse, 0 = forward @n
 * Bit 2: 1 = JOG, 0 = stop JOG @n
 * Bit 3: 1 = fault reset 0 = no reset @n
 */
#define VFD_INSTRUCTION         0x0900

/** Write frequency in 0.01 Hz steps */
#define VFD_FREQUENCY           0x0901

/** Running states the vfd can be in. */
enum vfd_state {
    VFD_STOP = 0,
    VFD_CW = 1,
    VFD_CCW = 3,
};

/** Signals, pins and parameters from LinuxCNC and HAL */
struct haldata {
    /* Information acquired from vfd */
    hal_s32_t   *inverter_status;   /*!< vfd's running state */
    hal_float_t *freq_cmd;          /*!< reference frequency (Hz) */
    hal_float_t *output_freq;       /*!< output frequency (Hz) */
    hal_float_t *output_current;    /*!< motor current (A) */
    hal_float_t *output_volt;       /*!< motor voltage (V) */
    hal_s32_t   *dc_bus_volt;       /*!< main voltage (V) */
    hal_float_t *motor_load;
    hal_s32_t   *inverter_temp;
    hal_bit_t   *vfd_error;
    hal_bit_t   *at_speed;
    hal_bit_t   *is_stopped;
    hal_float_t *speed_fb;

    /* Commands from LinuxCNC */
    hal_bit_t   *spindle_on;
    hal_bit_t   *spindle_fwd;
    hal_bit_t   *spindle_rev;
    hal_float_t *speed_cmd;

    /* Parameters */
    hal_float_t speed_tolerance;
    hal_float_t period;
    hal_s32_t   modbus_errors;
};

static int done;
char *modname = "nowforever_vfd";

static int read_data(modbus_t *mb_ctx, struct haldata *hal_data_block)
{
    int retries;
    uint16_t receive_data[MODBUS_MAX_READ_REGISTERS];

    for (retries = 0; retries <= NUM_MODBUS_RETRIES; retries++) {
        int retval = modbus_read_registers(mb_ctx, START_REGISTER_READ,
                                           NUM_REGISTER_READ, receive_data);

        if (retval == NUM_REGISTER_READ) {
            *hal_data_block->inverter_status = receive_data[0];
            *hal_data_block->freq_cmd = receive_data[1] * 0.01;
            *hal_data_block->output_freq = receive_data[2] * 0.01;
            *hal_data_block->output_current = receive_data[3] * 0.1;
            *hal_data_block->output_volt = receive_data[4] * 0.1;
            *hal_data_block->dc_bus_volt = receive_data[5];
            *hal_data_block->motor_load = receive_data[6] * 0.1;
            *hal_data_block->inverter_temp = receive_data[7];
            return 0;
        }
        fprintf(stderr, "%s: ERROR reading data for %d registers, from register 0x%04x: %s\n",
                modname, NUM_REGISTER_READ, START_REGISTER_READ,
                modbus_strerror(errno));
        hal_data_block->modbus_errors++;
    }
    return -1;
}

/**
 * @brief Set new state for vfd.
 *
 * Possible states is @c CW, @c CCW and @c STOP, it will only write to the
 * inverter if a new state has been requested.
 *
 * @param mb_ctx modbus context
 * @param haldata Information to and from LinuxCNC.
 * @return 0 on success and when we continue with the current state.
 *         Otherwise return -1.
 */
static int set_vfd_state(modbus_t *mb_ctx, struct haldata *haldata)
{
    int retries;
    uint16_t state;

    if (*haldata->spindle_on && *haldata->spindle_fwd &&
       (*haldata->inverter_status & 3) != VFD_CW) {
        state = VFD_CW;
    } else if (*haldata->spindle_on && *haldata->spindle_rev &&
              (*haldata->inverter_status & 3) != VFD_CCW) {
        state = VFD_CCW;
    } else if (!*haldata->spindle_on && (*haldata->inverter_status & 1) != VFD_STOP) {
        state = VFD_STOP;
    /* No new state has been requested. */
    } else {
        return 0;
    }

    for (retries = 0; retries <= NUM_MODBUS_RETRIES; retries++) {
        if (modbus_write_registers(mb_ctx, VFD_INSTRUCTION, 0x01, &state) == 1)
            return 0;
        fprintf(stderr, "%s: ERROR writing %u to register 0x%04x: %s\n",
                modname, state, VFD_INSTRUCTION, modbus_strerror(errno));
        haldata->modbus_errors++;
    }
    return -1;
}

/**
 * @brief Write new frequency to vfd.
 *
 * If the new frequency is different from the current frequency, send
 * the new frequency to vfd. If the frequency is identical, do nothing.
 * Ensures that the frequency written to vfd is a positive number, and that the
 * frequency is never larger than @c max_freq.
 *
 * @param mb_ctx modbus context
 * @param haldata Information to and from LinuxCNC.
 * @param freq_calc Calculated value, based on @c max_freq and
 *                  @c spindle_max_speed
 * @param max_freq Maximum allowed frequency.
 * @return 0 on success, or when it's not needed to write data to vfd.
 *         Otherwise return -1.
 */
static int set_vfd_freq(modbus_t *mb_ctx, struct haldata *haldata,
                        double freq_calc, double max_freq)
{
    int retries;
    uint16_t freq;

    /* Ensure frequency is a positive number */
    freq = abs((int) (*haldata->speed_cmd * freq_calc * 100));

    /* Cap at max frequency */
    if (freq > max_freq * 100)
        freq = (uint16_t) (max_freq * 100);

    /* Cast to int, to compare values. */
    if (freq == (int) (*haldata->output_freq * 100))
        return 0;

    for (retries = 0; retries <= NUM_MODBUS_RETRIES; retries++) {
        if (modbus_write_registers(mb_ctx, VFD_FREQUENCY, 0x01, &freq) == 1)
            return 0;
        fprintf(stderr, "%s: ERROR writing %u to register 0x%04x: %s\n",
                modname, freq, VFD_FREQUENCY, modbus_strerror(errno));
        haldata->modbus_errors++;
    }
    return -1;
}

/* Write to vfd and set HAL pins */
static void write_data(modbus_t *mb_ctx, struct haldata *haldata,
                       double hzcalc, double max_freq)
{
    set_vfd_state(mb_ctx, haldata);
    set_vfd_freq(mb_ctx, haldata, hzcalc, max_freq);

    if (*haldata->output_freq == 0) {
        *haldata->is_stopped = 1;
    } else {
        *haldata->is_stopped = 0;
    }

    *haldata->speed_fb = *haldata->output_freq / hzcalc;

    /* Calculates in % difference between set and actual frequency */
    if (fabs(1 - (*haldata->freq_cmd / *haldata->output_freq)) < haldata->speed_tolerance) {
        *haldata->at_speed = 1;
    } else {
        *haldata->at_speed = 0;
    }

    if (*haldata->spindle_on == 0)
        *haldata->at_speed = 0;

    if ((*haldata->inverter_status & 24) != 0)
        *haldata->vfd_error = 1;
}

/* Command-line options */
static struct option long_options[] = {
    {"device", 1, 0, 'd'},
    {"name", 1, 0 , 'n'},
    {"parity", 1, 0, 'p'},
    {"rate", 1, 0, 'r'},
    {"verbose", 0, 0, 'v'},
    {"target", 1, 0, 't'},
    {"help", 0, 0, 'h'},
    {"spindle-max-speed", 1, 0, 'S'},
    {"max-frequency", 1, 0, 'F'},
    {0,0,0,0}
};

static char *option_string = "d:n:p:r:vt:hS:F:";

static char *paritystrings[] = {"even", "odd", "none", NULL};
static char paritychars[] = {'E', 'O', 'N'};

static char *ratestrings[] = {"2400", "4800", "9600", "19200", "38400", NULL};

static void quit(int sig)
{
    done = 1;
}

static int match_string(char *string, char **matches)
{
    int which, match;
    unsigned int len;
    which = 0;
    match = -1;
    if ((matches == NULL) || (string == NULL)) return -1;
    len = strlen(string);
    while (matches[which] != NULL) {
        if ((!strncmp(string, matches[which], len)) && (len <= strlen(matches[which]))) {
            if (match >= 0) return -1;  /* Multiple matches */
            match = which;
        }
        ++which;
    }
    return match;
}

static void usage(char **argv)
{
    printf("Usage: %s [ARGUMENTS]\n", argv[0]);
    printf("\n");
    printf("This program interfaces the Nowforever D100/E100 VFD to the LinuxCNC HAL.\n");
    printf("\n");
    printf("Optional arguments:\n");
    printf("   -d, --device <path> (default: /dev/ttyUSB0)\n");
    printf("       Set the name of the serial device to use\n");
    printf("   -n, --name <string> (default: nowforever_vfd)\n");
    printf("       Set the name of the HAL module.  The HAL comp name will be set to <string>, and all pin\n");
    printf("       and parameter names will begin with <string>.\n");
    printf("   -p, --parity {even,odd,none} (default: none)\n");
    printf("       Set serial parity to 'even', 'odd', or 'none'.\n");
    printf("   -r, --rate <n> (default: 19200)\n");
    printf("       Set baud rate to <n>. It is an error if the rate is not one of the following:\n");
    printf("       2400, 4800, 9600, 19200, 38400\n");
    printf("   -t, --target <n> (default: 1)\n");
    printf("       Set Modbus target number. This must match the device\n");
    printf("       number you set on the Nowforever VFD.\n");
    printf("   -S, --spindle-max-speed <f> (default: 24000.0)\n");
    printf("       The spindle's max speed in RPM. This must match the spindle speed value\n");
    printf("        when it is at max frequency\n");
    printf("   -F, --max-frequency <f> (default: 400.0)\n");
    printf("       This is the maximum output frequency of the VFD in Hz. It should correspond\n");
    printf("       to the maximum output value configured in VFD register P0-007\n");
    printf("   -v, --verbose\n");
    printf("       Turn on verbose mode.\n");
    printf("   -h, --help\n");
    printf("       Show this help.\n");
}

/**
 * @brief Create HAL pins.
 * @param haldata Information to and from, LinuxCNC.
 * @param hal_comp_id Component ID created by HAL.
 * @return 0 on success, -1 on failure.
 */
static int hal_setup(struct haldata *haldata, int hal_comp_id)
{
    int retval;

    retval = hal_pin_s32_newf(HAL_OUT, &haldata->inverter_status,
                              hal_comp_id, "%s.inverter-status", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_OUT, &haldata->freq_cmd,
                                hal_comp_id, "%s.frequency-command", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_OUT, &haldata->output_freq,
                                hal_comp_id, "%s.frequency-out", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_OUT, &haldata->output_current,
                                hal_comp_id, "%s.output-current", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_OUT, &haldata->output_volt,
                                hal_comp_id, "%s.output-volt", modname);
    if (retval != 0) return retval;

    retval = hal_pin_s32_newf(HAL_OUT, &haldata->dc_bus_volt,
                              hal_comp_id, "%s.DC-bus-volt", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_OUT, &haldata->motor_load,
                                hal_comp_id, "%s.load-percentage", modname);
    if (retval != 0) return retval;

    retval = hal_pin_s32_newf(HAL_OUT, &haldata->inverter_temp,
                              hal_comp_id, "%s.inverter-temp", modname);
    if (retval != 0) return retval;

    retval = hal_pin_bit_newf(HAL_OUT, &haldata->vfd_error,
                              hal_comp_id, "%s.vfd-error", modname);
    if (retval != 0) return retval;

    retval = hal_pin_bit_newf(HAL_OUT, &haldata->at_speed,
                              hal_comp_id, "%s.at-speed", modname);
    if (retval != 0) return retval;

    retval = hal_pin_bit_newf(HAL_OUT, &haldata->is_stopped,
                              hal_comp_id, "%s.is-stopped", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_OUT, &haldata->speed_fb,
                                hal_comp_id, "%s.spindle-speed-fb", modname);
    if (retval != 0) return retval;

    retval = hal_pin_bit_newf(HAL_IN, &haldata->spindle_on,
                              hal_comp_id, "%s.spindle-on", modname);
    if (retval != 0) return retval;

    retval = hal_pin_bit_newf(HAL_IN, &haldata->spindle_fwd,
                              hal_comp_id, "%s.spindle-fwd", modname);
    if (retval != 0) return retval;

    retval = hal_pin_bit_newf(HAL_IN, &haldata->spindle_rev,
                              hal_comp_id, "%s.spindle-rev", modname);
    if (retval != 0) return retval;

    retval = hal_pin_float_newf(HAL_IN, &haldata->speed_cmd,
                                hal_comp_id, "%s.speed-command", modname);
    if (retval != 0) return retval;

    retval = hal_param_float_newf(HAL_RW, &haldata->speed_tolerance,
                                  hal_comp_id, "%s.tolerance", modname);
    if (retval != 0) return retval;

    retval = hal_param_float_newf(HAL_RW, &haldata->period,
                                  hal_comp_id, "%s.period-seconds", modname);
    if (retval != 0) return retval;

    retval = hal_param_s32_newf(HAL_RO, &haldata->modbus_errors,
                                hal_comp_id, "%s.modbus-errors", modname);
    if (retval != 0) return retval;

    return retval;
}

int main(int argc, char **argv)
{
    struct haldata *haldata;
    struct timespec period_timespec;

    modbus_t *mb_ctx;
    char *device;
    char parity;
    int baud;
    int bits;
    int stopbits;
    int target;
    int verbose;

    int retval = 0;
    int hal_comp_id;
    double spindle_max_speed = 24000.0;
    double max_freq = 400.0;
    double hzcalc;

    char *endarg;
    int opt;
    int argindex, argvalue;

    done = 0;

    /* Assume that nothing is specified on the command line */
    device = "/dev/ttyUSB0";
    baud = 19200;
    bits = 8;
    parity = 'N';
    stopbits = 1;

    verbose = 0;

    target = 1;

    /* Process command line options */
    while ((opt = getopt_long(argc, argv, option_string, long_options, NULL)) != -1) {
        switch (opt) {
            /* Device name, default /dev/ttyUSB0 */
            case 'd':
                /*
                 * Could check the device name here, but we'll leave it to
                 * the library open
                 */
                if (strlen(optarg) > FILENAME_MAX) {
                    fprintf(stderr, "ERROR: device node name is to long: %s\n",
                            optarg);
                    retval = -1;
                    goto out_noclose;
                }
                device = strdup(optarg);
                break;
            /* Module base name */
            case 'n':
                if (strlen(optarg) > HAL_NAME_LEN - 20) {
                    fprintf(stderr, "ERROR: HAL module name to long: %s\n",
                            optarg);
                    retval = -1;
                    goto out_noclose;
                }
                modname = strdup(optarg);
                break;
            /* Parity, should be a string like "even", "odd" or "none" */
            case 'p':
                argindex = match_string(optarg, paritystrings);
                if (argindex < 0) {
                    fprintf(stderr, "ERROR: invalid parity: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                parity = paritychars[argindex];
                break;
            /* Baud rate, defaults to 19200 */
            case 'r':
                argindex = match_string(optarg, ratestrings);
                if (argindex < 0) {
                    fprintf(stderr, "ERROR: invalid baud rate: %s\n", optarg);
                    retval = -1;
                    goto out_noclose;
                }
                baud = atoi(ratestrings[argindex]);
                break;
            /* Target number (MODBUS ID), default 1 */
            case 't':
                argvalue = strtol(optarg, &endarg, 10);
                if ((*endarg != '\0') || (argvalue < 1) || (argvalue > 31)) {
                    fprintf(stderr, "ERROR: invalid target number: %s\n",
                            optarg);
                    retval = -1;
                    goto out_noclose;
                }
                target = argvalue;
                break;
            case 'S':
                spindle_max_speed = strtod(optarg, &endarg);
                if ((*endarg != '\0') || (spindle_max_speed <= 0.0)) {
                    fprintf(stderr, "%s: ERROR: invalid spindle max speed: %s\n",
                            modname, optarg);
                    retval = -1;
                    goto out_noclose;
                }
                break;
            case 'F':
                max_freq = strtod(optarg, &endarg);
                if ((*endarg != '\0') || (max_freq <= 0.0)) {
                    fprintf(stderr, "%s: ERROR: invalid max frequency: %s\n",
                            modname, optarg);
                    retval = -1;
                    goto out_noclose;
                }
                break;
            case 'v':
                verbose = 1;
                break;
            case 'h':
                usage(argv);
                exit(0);
                break;
            default:
                usage(argv);
                exit(1);
                break;
        }
    }

    printf("%s: device='%s', baud='%d', bits=%d, parity='%c', stopbits=%d, address=%d\n",
            modname, device, baud, bits, parity, stopbits, target);

    /*
     * Point TERM and INT signals at our quit function.
     * If a signal is received between here and the main loop, it should
     * prevent some initialization from happening.
     */
    signal(SIGINT, quit);
    signal(SIGTERM, quit);

    /* Assume 19200 bps 8-N-1 serial setting, device 1 */
    mb_ctx = modbus_new_rtu(device, baud, parity, bits, stopbits);
    if (mb_ctx == NULL) {
        fprintf(stderr, "%s: ERROR: Couldn't open modbus serial device: %s\n",
                modname, modbus_strerror(errno));
        goto out_noclose;
    }

    retval = modbus_connect(mb_ctx);
    if (retval != 0) {
        fprintf(stderr, "%s: ERROR: Couldn't open serial device: %s\n",
                modname, modbus_strerror(errno));
        goto out_noclose;
    }

    modbus_set_debug(mb_ctx, verbose);
    modbus_set_slave(mb_ctx, target);

    /* Create HAL component */
    hal_comp_id = hal_init(modname);
    if (hal_comp_id < 0) {
        fprintf(stderr, "%s: ERROR: hal_init failed\n", modname);
        retval = hal_comp_id;
        goto out_close;
    }

    haldata = hal_malloc(sizeof(struct haldata));
    if (haldata == NULL) {
        fprintf(stderr, "%s: ERROR: unable to allocate shared memory\n",
                modname);
        retval = -1;
        goto out_closeHAL;
    }

    if (hal_setup(haldata, hal_comp_id)) {
        retval = -1;
        goto out_closeHAL;
    }

    /* Make default data match what we expect to use */
    *haldata->inverter_status = 0;
    *haldata->freq_cmd = 0.0;
    *haldata->output_freq = 0.0;
    *haldata->output_current = 0.0;
    *haldata->output_volt = 0.0;
    *haldata->dc_bus_volt = 0;
    *haldata->motor_load = 0.0;
    *haldata->inverter_temp = 0;
    *haldata->vfd_error = 0;

    *haldata->at_speed = 0;
    *haldata->is_stopped = 0;
    *haldata->speed_cmd = 0;

    haldata->speed_tolerance = 0.01;
    haldata->period = 0.1;
    haldata->modbus_errors = 0;

    /* Activate HAL component */
    hal_ready(hal_comp_id);

    /* Calculate frequency */
    hzcalc = max_freq / spindle_max_speed;

    while (done == 0) {
        /* Don't scan to fast, and not delay more than a few seconds */
        if (haldata->period < 0.001) haldata->period = 0.001;
        if (haldata->period > 2.0) haldata->period = 2.0;
        period_timespec.tv_sec = (time_t)(haldata->period);
        period_timespec.tv_nsec = (long)((haldata->period - period_timespec.tv_sec) * 1000000000l);
        nanosleep(&period_timespec, NULL);

        read_data(mb_ctx, haldata);
        write_data(mb_ctx, haldata, hzcalc, max_freq);
    }

    /* If we get here, then everything is fine, so just clean up and exit */
    retval = 0;
out_closeHAL:
    hal_exit(hal_comp_id);
out_close:
    modbus_close(mb_ctx);
    modbus_free(mb_ctx);
out_noclose:
    free(device);
    free(modname);
    return retval;
}
