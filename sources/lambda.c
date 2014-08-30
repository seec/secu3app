/* SECU-3  - An open source, free engine control unit
   Copyright (C) 2007 Alexey A. Shabelnikov. Ukraine, Kiev

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   contacts:
              http://secu-3.org
              email: shabelnikov@secu-3.org
*/

/** \file lambda.c
 * Implementation of correction algorithms using an exhaust gas oxygen sensor
 * (���������� ������������� ������� ����� ��������� ������ ���������).
 */

#ifdef FUEL_INJECT

#include "port/port.h"
#include "funconv.h"
#include "ignlogic.h"
#include "ioconfig.h"
#include "secu3.h"
#include "vstimer.h"

/**Lambda correction afterstart turn on timeout, 30 seconds*/
#define LAMBDA_AFTERSTART_TIMEOUT 3000

typedef struct
{
 uint8_t stroke_counter;         //!< Used to count strokes for correction integration
 uint16_t lambda_t1;             //!< timer
 uint8_t enabled;                //!< Flag indicates that lambda correction is enabled by timeout
}lambda_state_t;

/**Instance of internal state variables structure*/
static lambda_state_t ego;

void lambda_init_state(void)
{
 ego.stroke_counter = 0;
 ego.enabled = 0;
}

void lambda_control(struct ecudata_t* d)
{
 if (d->engine_mode == EM_START)
 {
  ego.lambda_t1 = s_timer_gtc();
  ego.enabled = 0;
  d->corr.lambda = 0;
 }
 else
 {
  if ((s_timer_gtc() - ego.lambda_t1) >= LAMBDA_AFTERSTART_TIMEOUT)
   ego.enabled = 1;
 }
}

void lambda_stroke_event_notification(struct ecudata_t* d)
{
 if (!IOCFG_CHECK(IOP_LAMBDA))
  return; //EGO is not enabled (input was not remapped)

 if (!ego.enabled)
  return; //wait some time before oxygen sensor will be turned on

 if (d->sens.inst_frq > d->param.inj_lambda_rpm_thrd)    //RPM > threshold
 {
  if (d->sens.temperat > d->param.inj_lambda_temp_thrd)  //coolant temperature > threshold
  {
   if (ego.stroke_counter)
    ego.stroke_counter--;
   else
   {
    ego.stroke_counter = d->param.inj_lambda_str_per_stp;

    //update EGO correction
    if (d->sens.add_i1 > d->param.inj_lambda_swt_point)
     d->corr.lambda-=d->param.inj_lambda_step_size;
    else if (d->sens.add_i1 < d->param.inj_lambda_swt_point)
     d->corr.lambda+=d->param.inj_lambda_step_size;

    restrict_value_to(&d->corr.lambda, -d->param.inj_lambda_corr_limit, d->param.inj_lambda_corr_limit);
   }
  }
  else
   d->corr.lambda = 0;
 }
 else
  d->corr.lambda = 0;
}

#endif