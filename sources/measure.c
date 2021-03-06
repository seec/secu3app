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

/** \file measure.c
 * \author Alexey A. Shabelnikov
 * Implementation pf processing (averaging, corrections etc) of data comes from ADC and sensors
 * (���������� ��������� (����������, ������������� � �.�.) ������ ����������� �� ��� � ��������).
 */

#include "port/avrio.h"
#include "port/interrupt.h"
#include "port/intrinsic.h"
#include "port/port.h"
#include <stdlib.h>
#include "bitmask.h"
#include "ecudata.h"
#include "eculogic.h"
#include "spdsens.h"
#include "funconv.h"    //thermistor_lookup(), ats_lookup
#include "ioconfig.h"
#include "magnitude.h"
#include "measure.h"

#ifdef VREF_5V //voltage divider is not necessary when ref. voltage is 5V
 /**Special macro for compensating of voltage division (without voltage divider)*/
 #define _RESDIV(v, n, d) (v)
#else //voltage divider is used
 /**Special macro for compensating of voltage division (with voltage divider)*/
 #define _RESDIV(v, n, d) (((n) * (v)) / (d))
#endif

/**Reads state of throttle gate (only the value, without inversion)
 * ��������� ��������� ����������� �������� (������ ��������, ��� ��������)
 */
#define GET_THROTTLE_GATE_STATE() (CHECKBIT(PINA, PINA7) > 0)

/**Number of values for averaging of RPM for tachometer
 * ���-�� �������� ��� ���������� ������� �������� �.�. ��� �������� ��������� */
#define FRQ_AVERAGING           4

//������ ������� ���������� �� ������� ����������� �������
#define MAP_AVERAGING           4                 //!< Number of values for averaging of pressure (MAP)
#define BAT_AVERAGING           4                 //!< Number of values for averaging of board voltage
#define TMP_AVERAGING           8                 //!< Number of values for averaging of coolant temperature
#define TPS_AVERAGING           4                 //!< Number of values for averaging of throttle position
#define AI1_AVERAGING           4                 //!< Number of values for averaging of ADD_IO1
#define AI2_AVERAGING           4                 //!< Number of values for averaging of ADD_IO2
#ifdef SPEED_SENSOR
#define SPD_AVERAGING           8                 //!< Number of values for averaging of speed sensor periods
#endif

uint16_t freq_circular_buffer[FRQ_AVERAGING];     //!< Ring buffer for RPM averaging for tachometer (����� ���������� ������� �������� ��������� ��� ���������)
uint16_t map_circular_buffer[MAP_AVERAGING];      //!< Ring buffer for averaging of MAP sensor (����� ���������� ����������� ��������)
uint16_t ubat_circular_buffer[BAT_AVERAGING];     //!< Ring buffer for averaging of voltage (����� ���������� ���������� �������� ����)
uint16_t temp_circular_buffer[TMP_AVERAGING];     //!< Ring buffer for averaging of coolant temperature (����� ���������� ����������� ����������� ��������)
uint16_t tps_circular_buffer[TPS_AVERAGING];      //!< Ring buffer for averaging of TPS
uint16_t ai1_circular_buffer[AI1_AVERAGING];      //!< Ring buffer for averaging of ADD_IO1
uint16_t ai2_circular_buffer[AI2_AVERAGING];      //!< Ring buffer for averaging of ADD_IO2
#ifdef SPEED_SENSOR
uint16_t spd_circular_buffer[SPD_AVERAGING];      //!< Ring buffer for averaging of speed sensor periods
#endif

void meas_init_ports(void)
{
 IOCFG_INIT(IOP_GAS_V, 0);    //don't use internal pullup resistor
 //We don't initialize analog inputs (ADD_I1, ADD_I2, CARB) because they are initialised by default
 //and we don't need pullup resistors for them
}

//���������� ������� ���������� (������� ��������, �������...)
void meas_update_values_buffers(struct ecudata_t* d, uint8_t rpm_only)
{
 static uint8_t  map_ai  = MAP_AVERAGING-1;
 static uint8_t  bat_ai  = BAT_AVERAGING-1;
 static uint8_t  tmp_ai  = TMP_AVERAGING-1;
 static uint8_t  frq_ai  = FRQ_AVERAGING-1;
 static uint8_t  tps_ai  = TPS_AVERAGING-1;
 static uint8_t  ai1_ai  = AI1_AVERAGING-1;
 static uint8_t  ai2_ai  = AI2_AVERAGING-1;
#ifdef SPEED_SENSOR
 static uint8_t  spd_ai = SPD_AVERAGING-1;
#endif

 freq_circular_buffer[frq_ai] = d->sens.inst_frq;
 (frq_ai==0) ? (frq_ai = FRQ_AVERAGING - 1): frq_ai--;

 if (rpm_only)
  return;

 map_circular_buffer[map_ai] = adc_get_map_value();
 (map_ai==0) ? (map_ai = MAP_AVERAGING - 1): map_ai--;

 ubat_circular_buffer[bat_ai] = adc_get_ubat_value();
 (bat_ai==0) ? (bat_ai = BAT_AVERAGING - 1): bat_ai--;

 temp_circular_buffer[tmp_ai] = adc_get_temp_value();
 (tmp_ai==0) ? (tmp_ai = TMP_AVERAGING - 1): tmp_ai--;

 tps_circular_buffer[tps_ai] = adc_get_carb_value();
 (tps_ai==0) ? (tps_ai = TPS_AVERAGING - 1): tps_ai--;

 ai1_circular_buffer[ai1_ai] = adc_get_add_io1_value();
 (ai1_ai==0) ? (ai1_ai = AI1_AVERAGING - 1): ai1_ai--;

 ai2_circular_buffer[ai2_ai] = adc_get_add_io2_value();
 (ai2_ai==0) ? (ai2_ai = AI2_AVERAGING - 1): ai2_ai--;

 if (d->param.knock_use_knock_channel)
 {
#ifdef VREF_5V
  d->sens.knock_k = adc_compensate(adc_get_knock_value(), ADC_COMP_FACTOR(ADC_VREF_FACTOR), ADC_COMP_CORR(ADC_VREF_FACTOR, 0.0));
#else //internal 2.56V
  d->sens.knock_k = adc_get_knock_value() * 2;
#endif
 }
 else
  d->sens.knock_k = 0; //knock signal value must be zero if knock detection turned off

#ifdef SPEED_SENSOR
 spd_circular_buffer[spd_ai] = spdsens_get_period();
 (spd_ai==0) ? (spd_ai = SPD_AVERAGING - 1): spd_ai--;
 d->sens.distance = spdsens_get_pulse_count();
#endif

#ifdef FUEL_INJECT
 if (d->engine_mode != EM_START)
 {
  d->sens.tpsdot = adc_compensate(_RESDIV(adc_get_tpsdot_value(), 2, 1), d->param.tps_adc_factor, 0);
  d->sens.tpsdot = tpsdot_adc_to_pc(d->sens.tpsdot, d->param.tps_curve_gradient);
 }
 else
  d->sens.tpsdot = 0; //disable accel.enrichment during cranking
#endif
}


//���������� ���������� ������� ��������� ������� �������� ��������� ������� ����������, �����������
//������������ ���, ������� ���������� �������� � ���������� ��������.
void meas_average_measured_values(struct ecudata_t* d)
{
 uint8_t i;  uint32_t sum;
 static uint16_t temp_avr = 0;

 for (sum=0,i = 0; i < MAP_AVERAGING; i++)  //��������� �������� � ������� ����������� ��������
  sum+=map_circular_buffer[i];
 d->sens.map_raw = adc_compensate(_RESDIV((sum/MAP_AVERAGING), 2, 1), d->param.map_adc_factor, d->param.map_adc_correction);
 d->sens.map = map_adc_to_kpa(d->sens.map_raw, d->param.map_curve_offset, d->param.map_curve_gradient);

 for (sum=0,i = 0; i < BAT_AVERAGING; i++)   //��������� ���������� �������� ����
  sum+=ubat_circular_buffer[i];
 d->sens.voltage_raw = adc_compensate((sum/BAT_AVERAGING) * 6, d->param.ubat_adc_factor,d->param.ubat_adc_correction);
 d->sens.voltage = ubat_adc_to_v(d->sens.voltage_raw);

 if (d->param.tmp_use)
 {
  for (sum=0,i = 0; i < TMP_AVERAGING; i++) //��������� ����������� (����)
  { //filter noisy samples by comparing each sample with averaged value (threshold is 6.25%)
   uint16_t t = (temp_avr >> 4);
   if (temp_avr && (temp_circular_buffer[i] > (temp_avr + t)))
    sum+=(temp_avr + t);
   else if (temp_avr && (temp_circular_buffer[i] < ((int16_t)temp_avr - t)))
    sum+=(temp_avr - t);
   else
    sum+=temp_circular_buffer[i];
  }
  temp_avr = sum/TMP_AVERAGING;
  d->sens.temperat_raw = adc_compensate(_RESDIV(temp_avr, 5, 3),d->param.temp_adc_factor,d->param.temp_adc_correction);
#ifndef THERMISTOR_CS
  d->sens.temperat = temp_adc_to_c(d->sens.temperat_raw);
#else
  if (!d->param.cts_use_map) //use linear sensor
   d->sens.temperat = temp_adc_to_c(d->sens.temperat_raw);
  else //use lookup table (actual for thermistor sensors)
   d->sens.temperat = thermistor_lookup(d->sens.temperat_raw);
#endif
 }
 else                                       //���� �� ������������
  d->sens.temperat = 0;

 for (sum=0,i = 0; i < FRQ_AVERAGING; i++)  //��������� ������� �������� ���������
  sum+=freq_circular_buffer[i];
 d->sens.frequen=(sum/FRQ_AVERAGING);

#ifdef SPEED_SENSOR
 for (sum=0,i = 0; i < SPD_AVERAGING; i++)  //average periods from speed sensor
  sum+=spd_circular_buffer[i];
 d->sens.speed=(sum/SPD_AVERAGING);
#endif

 for (sum=0,i = 0; i < TPS_AVERAGING; i++)   //average throttle position
  sum+=tps_circular_buffer[i];
 d->sens.tps_raw = adc_compensate(_RESDIV((sum/TPS_AVERAGING), 2, 1), d->param.tps_adc_factor, d->param.tps_adc_correction);
 d->sens.tps = tps_adc_to_pc(d->sens.tps_raw, d->param.tps_curve_offset, d->param.tps_curve_gradient);
 if (d->sens.tps > TPS_MAGNITUDE(100))
  d->sens.tps = TPS_MAGNITUDE(100);

 for (sum=0,i = 0; i < AI1_AVERAGING; i++)   //average ADD_IO1 input
  sum+=ai1_circular_buffer[i];
 d->sens.add_i1_raw = adc_compensate(_RESDIV((sum/AI1_AVERAGING), 2, 1), d->param.ai1_adc_factor, d->param.ai1_adc_correction);
 d->sens.add_i1 = d->sens.add_i1_raw;

 for (sum=0,i = 0; i < AI2_AVERAGING; i++)   //average ADD_IO2 input
  sum+=ai2_circular_buffer[i];
 d->sens.add_i2_raw = adc_compensate(_RESDIV((sum/AI2_AVERAGING), 2, 1), d->param.ai2_adc_factor, d->param.ai2_adc_correction);
 d->sens.add_i2 = d->sens.add_i2_raw;

#ifdef AIRTEMP_SENS
 if (IOCFG_CHECK(IOP_AIR_TEMP))
  d->sens.air_temp = ats_lookup(d->sens.add_i2_raw);   //ADD_IO2 input
 else
  d->sens.air_temp = 0; //input is not selected
#endif
}

//�������� ��� ���������������� ��������� ����� ������ ���������. �������� ������ �����
//������������� ���.
void meas_initial_measure(struct ecudata_t* d)
{
 uint8_t _t,i = 16;
 _t = _SAVE_INTERRUPT();
 _ENABLE_INTERRUPT();
 do
 {
  adc_begin_measure(0); //<--normal speed
  while(!adc_is_measure_ready());

  meas_update_values_buffers(d, 0); //<-- all
 }while(--i);
 _RESTORE_INTERRUPT(_t);
 meas_average_measured_values(d);
}


#ifdef REALTIME_TABLES
/** Selects set of tables specified by index, sets corresponding flag depending on where tables' set resides: RAM or FLASH
 * \param d Pointer to ECU data structure
 * \param set_index Index of tables' set to be selected
 */
static void select_table_set(struct ecudata_t *d, uint8_t set_index)
{
 if (set_index > (TABLES_NUMBER_PGM-1))
 {
  d->mm_ptr8 = mm_get_byte_ram;
  d->mm_ptr16 = mm_get_word_ram;
 }
 else
 {
  d->fn_dat = &fw_data.tables[set_index];
  d->mm_ptr8 = mm_get_byte_pgm;
  d->mm_ptr16 = mm_get_word_pgm;
 }
}
#endif


void meas_take_discrete_inputs(struct ecudata_t *d)
{
 //--�������� ��������� ����������� ���� ����������
 if (0==d->param.tps_threshold)
  d->sens.carb=d->param.carb_invers^GET_THROTTLE_GATE_STATE(); //���������: 0 - �������� ������, 1 - ������
 else
 {//using TPS (������������� � ����)
  d->sens.carb=d->param.carb_invers^(d->sens.tps > d->param.tps_threshold);
 }

 //read state of gas valve input (��������� � ��������� ��������� �������� �������)
 //if GAS_V input remapped to other function, then petrol
 d->sens.gas = IOCFG_GET(IOP_GAS_V);

 //����������� ��� ������� � ����������� �� ��������� �������� ������� � ��������������� ����� (���� ������������)
#ifndef REALTIME_TABLES
 if (!IOCFG_CHECK(IOP_MAPSEL0))
 { //without additioanl selection input
  if (d->sens.gas)
   d->fn_dat = &fw_data.tables[d->param.fn_gas];     //�� ����
  else
   d->fn_dat = &fw_data.tables[d->param.fn_gasoline];//�� �������
 }
 else
 { //use! additional selection input
  uint8_t mapsel0 = IOCFG_GET(IOP_MAPSEL0);
  if (d->sens.gas) //�� ����
   d->fn_dat = mapsel0 ? &fw_data.tables[1] : &fw_data.tables[d->param.fn_gas];
  else             //�� �������
   d->fn_dat = mapsel0 ? &fw_data.tables[0] : &fw_data.tables[d->param.fn_gasoline];
 }
#else //use tables from RAM

 if (!IOCFG_CHECK(IOP_MAPSEL0))
 { //without additioanl selection input
   select_table_set(d, d->sens.gas ? d->param.fn_gas : d->param.fn_gasoline);   //gas/petrol
 }
 else
 { //use! additional selection input
  uint8_t mapsel0 = IOCFG_GET(IOP_MAPSEL0);
  if (d->sens.gas)
   select_table_set(d, mapsel0 ? 1 : d->param.fn_gas);          //on gas
  else
   select_table_set(d, mapsel0 ? 0 : d->param.fn_gasoline);     //on petrol
 }
#endif
}
