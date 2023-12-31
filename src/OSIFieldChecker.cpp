//
// Copyright 2018 PMSF IT Consulting - Pierre R. Mai
// Copyright 2023 BMW AG
// SPDX-License-Identifier: MPL-2.0
//

#include "OSIFieldChecker.h"

/*
 * Debug Breaks
 *
 * If you define DEBUG_BREAKS the FMU will automatically break
 * into an attached Debugger on all major computation functions.
 * Note that the FMU is likely to break all environments if no
 * Debugger is actually attached when the breaks are triggered.
 */
#if defined(DEBUG_BREAKS) && !defined(NDEBUG)
#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define DEBUGBREAK() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define DEBUGBREAK() __debugbreak()
#endif
#endif
#if !defined(DEBUGBREAK)
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
#include <intrin.h>
#define DEBUGBREAK() __debugbreak()
#else
#include <signal.h>
#if defined(SIGTRAP)
#define DEBUGBREAK() raise(SIGTRAP)
#else
#define DEBUGBREAK() raise(SIGABRT)
#endif
#endif
#endif
#else
#define DEBUGBREAK()
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

using namespace std;

#ifdef PRIVATE_LOG_PATH
ofstream COSMPDummySensor::private_log_file;
#endif

/*
 * ProtocolBuffer Accessors
 */

void* DecodeIntegerToPointer(fmi2Integer hi, fmi2Integer lo)
{
#if PTRDIFF_MAX == INT64_MAX
  union Addrconv
  {
    struct
    {
      int lo;
      int hi;
    } base;
    unsigned long long address;
  } myaddr;
  myaddr.base.lo = lo;
  myaddr.base.hi = hi;
  return reinterpret_cast<void*>(myaddr.address);
#elif PTRDIFF_MAX == INT32_MAX
  return reinterpret_cast<void*>(lo);
#else
#error "Cannot determine 32bit or 64bit environment!"
#endif
}

void EncodePointerToInteger(const void* ptr, fmi2Integer& hi, fmi2Integer& lo)
{
#if PTRDIFF_MAX == INT64_MAX
  union Addrconv
  {
    struct
    {
      int lo;
      int hi;
    } base;
    unsigned long long address;
  } myaddr;
  myaddr.address = reinterpret_cast<unsigned long long>(ptr);
  hi = myaddr.base.hi;
  lo = myaddr.base.lo;
#elif PTRDIFF_MAX == INT32_MAX
  hi = 0;
  lo = reinterpret_cast<int>(ptr);
#else
#error "Cannot determine 32bit or 64bit environment!"
#endif
}

bool OSIFieldChecker::GetFmiSensorDataIn(osi3::SensorData& data)
{
  if (integer_vars_[FMI_INTEGER_SENSORDATA_IN_SIZE_IDX] > 0)
  {
    void* buffer = DecodeIntegerToPointer(integer_vars_[FMI_INTEGER_SENSORDATA_IN_BASEHI_IDX], integer_vars_[FMI_INTEGER_SENSORDATA_IN_BASELO_IDX]);
    NormalLog("OSMP", "Got %08X %08X, reading from %p ...", integer_vars_[FMI_INTEGER_SENSORDATA_IN_BASEHI_IDX], integer_vars_[FMI_INTEGER_SENSORDATA_IN_BASELO_IDX], buffer);
    data.ParseFromArray(buffer, integer_vars_[FMI_INTEGER_SENSORDATA_IN_SIZE_IDX]);
    return true;
  }
  return false;
}

void OSIFieldChecker::SetFmiSensorDataOut(const osi3::SensorData& data)
{
  data.SerializeToString(current_output_buffer_);
  EncodePointerToInteger(current_output_buffer_->data(), integer_vars_[FMI_INTEGER_SENSORDATA_OUT_BASEHI_IDX], integer_vars_[FMI_INTEGER_SENSORDATA_OUT_BASELO_IDX]);
  integer_vars_[FMI_INTEGER_SENSORDATA_OUT_SIZE_IDX] = (fmi2Integer)current_output_buffer_->length();
  NormalLog("OSMP",
            "Providing %08X %08X, writing from %p ...",
            integer_vars_[FMI_INTEGER_SENSORDATA_OUT_BASEHI_IDX],
            integer_vars_[FMI_INTEGER_SENSORDATA_OUT_BASELO_IDX],
            current_output_buffer_->data());
  swap(current_output_buffer_, last_output_buffer_);
}

void OSIFieldChecker::ResetFmiSensorDataOut()
{
  integer_vars_[FMI_INTEGER_SENSORDATA_OUT_SIZE_IDX] = 0;
  integer_vars_[FMI_INTEGER_SENSORDATA_OUT_BASEHI_IDX] = 0;
  integer_vars_[FMI_INTEGER_SENSORDATA_OUT_BASELO_IDX] = 0;
}

/*
 * Actual Core Content
 */

fmi2Status OSIFieldChecker::DoInit()
{
  /* Booleans */
  for (int& boolean_var : boolean_vars_)
  {
    boolean_var = fmi2False;
  }

  /* Integers */
  for (int& integer_var : integer_vars_)
  {
    integer_var = 0;
  }

  /* Reals */
  for (double& real_var : real_vars_)
  {
    real_var = 0.0;
  }

  /* Strings */
  for (auto& string_var : string_vars_)
  {
    string_var = "";
  }

  return fmi2OK;
}

/*fmi2Status OSICheck::DoStart(fmi2Boolean toleranceDefined, fmi2Real tolerance, fmi2Real startTime, fmi2Boolean stopTimeDefined, fmi2Real stopTime)
{
    return fmi2OK;
}*/

fmi2Status OSIFieldChecker::DoEnterInitializationMode()
{
  return fmi2OK;
}

fmi2Status OSIFieldChecker::DoExitInitializationMode()
{
  fstream osi_check_file;
  const std::string check_file = FmiCheckFile();
  osi_check_file.open(check_file, ios::in);  // open a file to perform read operation using file object
  if (osi_check_file.is_open())
  {  // checking whether the file is open
    string current_line;
    while (getline(osi_check_file, current_line))
    {  // read data from file object and put it into string.
      expected_osi_fields_.insert(current_line);
    }
    osi_check_file.close();  // close the file object.
  }
  else
  {
    std::cerr << "OSI check file not found!" << std::endl;
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::DoCalc(fmi2Real current_communication_point, fmi2Real communication_step_size)
{
  osi3::SensorData sensor_data_in;
  osi3::SensorData sensor_data_out;

  const double start_check_in_s = 0.5;

  if (GetFmiSensorDataIn(sensor_data_in) && current_communication_point > start_check_in_s)  // start checker after 0.5 s to give simulation models time to settle
  {
    if (!sensor_data_in.moving_object().empty() && expected_osi_fields_.find("moving_object") != expected_osi_fields_.end())
    {
      CheckMovingObjects(sensor_data_in, current_communication_point);
    }
    else
    {
      if (expected_osi_fields_.find("moving_object") != expected_osi_fields_.end())
      {
        missing_fields_.insert("moving_object");
        std::cout << current_communication_point << ": missing moving_object" << std::endl;
      }
    }

    /* Clear Output */
    sensor_data_out.Clear();
    sensor_data_out.CopyFrom(sensor_data_in);

    /* Serialize */
    SetFmiSensorDataOut(sensor_data_out);
    SetFmiValid(1);
    SetFmiCount(sensor_data_out.moving_object_size());
  }
  else
  {
    /* We have no valid input, so no valid output */
    NormalLog("OSI", "No valid input, therefore providing no valid output.");
    ResetFmiSensorDataOut();
    SetFmiValid(0);
    SetFmiCount(0);
  }
  return fmi2OK;
}

void OSIFieldChecker::CheckMovingObjects(const osi3::SensorData& sensor_data_in, const fmi2Real& current_communication_point)
{
  for (const auto& current_check : expected_osi_fields_)
  {
    if (current_check == "moving_object.base" && !sensor_data_in.moving_object(0).has_base())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.dimension" && sensor_data_in.moving_object(0).has_base() && !sensor_data_in.moving_object(0).base().has_dimension())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.position" && sensor_data_in.moving_object(0).has_base() && !sensor_data_in.moving_object(0).base().has_position())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.orientation" && sensor_data_in.moving_object(0).has_base() && !sensor_data_in.moving_object(0).base().has_orientation())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.velocity" && sensor_data_in.moving_object(0).has_base() && !sensor_data_in.moving_object(0).base().has_velocity())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.acceleration" && sensor_data_in.moving_object(0).has_base() && !sensor_data_in.moving_object(0).base().has_acceleration())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.orientation_rate" && sensor_data_in.moving_object(0).has_base() && !sensor_data_in.moving_object(0).base().has_orientation_rate())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.orientation_acceleration" && sensor_data_in.moving_object(0).has_base() &&
             !sensor_data_in.moving_object(0).base().has_orientation_acceleration())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
    else if (current_check == "moving_object.base.base_polygon" && sensor_data_in.moving_object(0).has_base() && sensor_data_in.moving_object(0).base().base_polygon().empty())
    {
      missing_fields_.insert(current_check);
      std::cout << current_communication_point << ": missing " << current_check << std::endl;
    }
  }
}

fmi2Status OSIFieldChecker::DoTerm()
{
  return fmi2OK;
}

/*void OSICheck::DoFree()
{
    DEBUGBREAK();
}*/

/*
 * Generic C++ Wrapper Code
 */

OSIFieldChecker::OSIFieldChecker(fmi2String theinstance_name,
                                 fmi2Type thefmu_type,
                                 fmi2String thefmu_guid,
                                 fmi2String thefmu_resource_location,
                                 const fmi2CallbackFunctions* thefunctions,
                                 fmi2Boolean thevisible,
                                 fmi2Boolean thelogging_on)
    : instance_name_(theinstance_name),
      fmu_type_(thefmu_type),
      fmu_guid_(thefmu_guid),
      fmu_resource_location_(thefmu_resource_location),
      functions_(*thefunctions),
      visible_(thevisible != 0),
      logging_on_(thelogging_on != 0),
      simulation_started_(false),
      current_output_buffer_(new string()),
      last_output_buffer_(new string())
{
  logging_categories_.clear();
  logging_categories_.insert("FMI");
  logging_categories_.insert("OSMP");
  logging_categories_.insert("OSI");
}

fmi2Status OSIFieldChecker::SetDebugLogging(fmi2Boolean thelogging_on, size_t n_categories, const fmi2String categories[])
{
  FmiVerboseLog("fmi2SetDebugLogging(%s)", thelogging_on != 0 ? "true" : "false");
  logging_on_ = thelogging_on != 0;
  if ((categories != nullptr) && (n_categories > 0))
  {
    logging_categories_.clear();
    for (size_t i = 0; i < n_categories; i++)
    {
      if (0 == strcmp(categories[i], "FMI"))
      {
        logging_categories_.insert("FMI");
      }
      else if (0 == strcmp(categories[i], "OSMP"))
      {
        logging_categories_.insert("OSMP");
      }
      else if (0 == strcmp(categories[i], "OSI"))
      {
        logging_categories_.insert("OSI");
      }
    }
  }
  else
  {
    logging_categories_.clear();
    logging_categories_.insert("FMI");
    logging_categories_.insert("OSMP");
    logging_categories_.insert("OSI");
  }
  return fmi2OK;
}

fmi2Component OSIFieldChecker::Instantiate(fmi2String instance_name,
                                           fmi2Type fmu_type,
                                           fmi2String fmu_guid,
                                           fmi2String fmu_resource_location,
                                           const fmi2CallbackFunctions* functions,
                                           fmi2Boolean visible,
                                           fmi2Boolean logging_on)
{
  auto* myc = new OSIFieldChecker(instance_name, fmu_type, fmu_guid, fmu_resource_location, functions, visible, logging_on);

  FmiVerboseLogGlobal(R"(fmi2Instantiate("%s",%d,"%s","%s","%s",%d,%d) = %p)",
                      instance_name,
                      fmu_type,
                      fmu_guid,
                      (fmu_resource_location != nullptr) ? fmu_resource_location : "<NULL>",
                      "FUNCTIONS",
                      visible,
                      logging_on,
                      myc);
  return (fmi2Component)myc;
}

fmi2Status OSIFieldChecker::EnterInitializationMode()
{
  FmiVerboseLog("fmi2EnterInitializationMode()");
  return DoEnterInitializationMode();
}

fmi2Status OSIFieldChecker::ExitInitializationMode()
{
  FmiVerboseLog("fmi2ExitInitializationMode()");
  simulation_started_ = true;
  return DoExitInitializationMode();
}

fmi2Status OSIFieldChecker::DoStep(fmi2Real current_communication_point, fmi2Real communication_step_size, fmi2Boolean no_set_fmu_state_prior_to_current_pointfmi2_component)
{
  FmiVerboseLog("fmi2DoStep(%g,%g,%d)", current_communication_point, communication_step_size, no_set_fmu_state_prior_to_current_pointfmi2_component);
  return DoCalc(current_communication_point, communication_step_size);
}

fmi2Status OSIFieldChecker::Terminate()
{
  FmiVerboseLog("fmi2Terminate()");

  int num_missing_fields = 0;

  if (!missing_fields_.empty())
  {
    for (const auto& current_missing_field : missing_fields_)
    {
      std::cout << "::error title=MissingField::" << current_missing_field << std::endl;
      num_missing_fields++;
    }
  }
  if (num_missing_fields > 0)
  {
    std::cout << "test failed" << std::endl;
    string output = "echo \"failed=" + to_string(1) + "\" >> $GITHUB_OUTPUT";
    system(output.c_str());
  }

  return DoTerm();
}

fmi2Status OSIFieldChecker::Reset()
{
  FmiVerboseLog("fmi2Reset()");

  // DoFree();
  simulation_started_ = false;
  return DoInit();
}

void OSIFieldChecker::FreeInstance()
{
  FmiVerboseLog("fmi2FreeInstance()");
  // DoFree();
}

fmi2Status OSIFieldChecker::GetReal(const fmi2ValueReference vr[], size_t nvr, fmi2Real value[])
{
  FmiVerboseLog("fmi2GetReal(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_REAL_VARS)
    {
      value[i] = real_vars_[vr[i]];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::GetInteger(const fmi2ValueReference vr[], size_t nvr, fmi2Integer value[])
{
  FmiVerboseLog("fmi2GetInteger(...)");
  // bool need_refresh = !simulation_started_;
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_INTEGER_VARS)
    {
      /*if (need_refresh && (vr[i] == FMI_INTEGER_SENSORVIEW_CONFIG_REQUEST_BASEHI_IDX || vr[i] == FMI_INTEGER_SENSORVIEW_CONFIG_REQUEST_BASELO_IDX || vr[i] ==
      FMI_INTEGER_SENSORVIEW_CONFIG_REQUEST_SIZE_IDX)) { refresh_fmi_sensor_view_config_request(); need_refresh = false;s
      }*/
      value[i] = integer_vars_[vr[i]];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::GetBoolean(const fmi2ValueReference vr[], size_t nvr, fmi2Boolean value[])
{
  FmiVerboseLog("fmi2GetBoolean(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_BOOLEAN_VARS)
    {
      value[i] = boolean_vars_[vr[i]];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::GetString(const fmi2ValueReference vr[], size_t nvr, fmi2String value[])
{
  FmiVerboseLog("fmi2GetString(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_STRING_VARS)
    {
      value[i] = string_vars_[vr[i]].c_str();
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::SetReal(const fmi2ValueReference vr[], size_t nvr, const fmi2Real value[])
{
  FmiVerboseLog("fmi2SetReal(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_REAL_VARS)
    {
      real_vars_[vr[i]] = value[i];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::SetInteger(const fmi2ValueReference vr[], size_t nvr, const fmi2Integer value[])
{
  FmiVerboseLog("fmi2SetInteger(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_INTEGER_VARS)
    {
      integer_vars_[vr[i]] = value[i];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::SetBoolean(const fmi2ValueReference vr[], size_t nvr, const fmi2Boolean value[])
{
  FmiVerboseLog("fmi2SetBoolean(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_BOOLEAN_VARS)
    {
      boolean_vars_[vr[i]] = value[i];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

fmi2Status OSIFieldChecker::SetString(const fmi2ValueReference vr[], size_t nvr, const fmi2String value[])
{
  FmiVerboseLog("fmi2SetString(...)");
  for (size_t i = 0; i < nvr; i++)
  {
    if (vr[i] < FMI_STRING_VARS)
    {
      string_vars_[vr[i]] = value[i];
    }
    else
    {
      return fmi2Error;
    }
  }
  return fmi2OK;
}

/*
 * FMI 2.0 Co-Simulation Interface API
 */

extern "C" {

FMI2_Export const char* fmi2GetTypesPlatform()
{
  return fmi2TypesPlatform;
}

FMI2_Export const char* fmi2GetVersion()
{
  return fmi2Version;
}

FMI2_Export fmi2Status fmi2SetDebugLogging(fmi2Component c, fmi2Boolean logging_on, size_t n_categories, const fmi2String categories[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->SetDebugLogging(logging_on, n_categories, categories);
}

/*
 * Functions for Co-Simulation
 */
FMI2_Export fmi2Component fmi2Instantiate(fmi2String instance_name,
                                          fmi2Type fmu_type,
                                          fmi2String fmu_guid,
                                          fmi2String fmu_resource_location,
                                          const fmi2CallbackFunctions* functions,
                                          fmi2Boolean visible,
                                          fmi2Boolean logging_on)
{
  return OSIFieldChecker::Instantiate(instance_name, fmu_type, fmu_guid, fmu_resource_location, functions, visible, logging_on);
}

FMI2_Export fmi2Status
fmi2SetupExperiment(fmi2Component c, fmi2Boolean tolerance_defined, fmi2Real tolerance, fmi2Real start_time, fmi2Boolean stop_time_defined, fmi2Real stop_time)
{
  return fmi2OK;
}

FMI2_Export fmi2Status fmi2EnterInitializationMode(fmi2Component c)
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->EnterInitializationMode();
}

FMI2_Export fmi2Status fmi2ExitInitializationMode(fmi2Component c)
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->ExitInitializationMode();
}

FMI2_Export fmi2Status fmi2DoStep(fmi2Component c,
                                  fmi2Real current_communication_point,
                                  fmi2Real communication_step_size,
                                  fmi2Boolean no_set_fmu_state_prior_to_current_pointfmi2_component)
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->DoStep(current_communication_point, communication_step_size, no_set_fmu_state_prior_to_current_pointfmi2_component);
}

FMI2_Export fmi2Status fmi2Terminate(fmi2Component c)
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->Terminate();
}

FMI2_Export fmi2Status fmi2Reset(fmi2Component c)
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->Reset();
}

FMI2_Export void fmi2FreeInstance(fmi2Component c)
{
  auto* myc = (OSIFieldChecker*)c;
  myc->FreeInstance();
  delete myc;
}

/*
 * Data Exchange Functions
 */
FMI2_Export fmi2Status fmi2GetReal(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2Real value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->GetReal(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2GetInteger(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2Integer value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->GetInteger(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2GetBoolean(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2Boolean value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->GetBoolean(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2GetString(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2String value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->GetString(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2SetReal(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Real value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->SetReal(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2SetInteger(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Integer value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->SetInteger(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2SetBoolean(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Boolean value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->SetBoolean(vr, nvr, value);
}

FMI2_Export fmi2Status fmi2SetString(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2String value[])
{
  auto* myc = (OSIFieldChecker*)c;
  return myc->SetString(vr, nvr, value);
}

/*
 * Unsupported Features (FMUState, Derivatives, Async DoStep, Status Enquiries)
 */
FMI2_Export fmi2Status fmi2GetFMUstate(fmi2Component c, fmi2FMUstate* fmu_state)
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2SetFMUstate(fmi2Component c, fmi2FMUstate fmu_state)
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2FreeFMUstate(fmi2Component c, fmi2FMUstate* fmu_state)
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2SerializedFMUstateSize(fmi2Component c, fmi2FMUstate fmu_state, size_t* size)
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2SerializeFMUstate(fmi2Component c, fmi2FMUstate fmu_state, fmi2Byte serialized_state[], size_t size)
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2DeSerializeFMUstate(fmi2Component c, const fmi2Byte serialized_state[], size_t size, fmi2FMUstate* fmu_state)
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2GetDirectionalDerivative(fmi2Component c,
                                                    const fmi2ValueReference v_unknown_ref[],
                                                    size_t n_unknown,
                                                    const fmi2ValueReference v_known_ref[],
                                                    size_t n_known,
                                                    const fmi2Real dv_known[],
                                                    fmi2Real dv_unknown[])
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2SetRealInputDerivatives(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Integer order[], const fmi2Real value[])
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2GetRealOutputDerivatives(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Integer order[], fmi2Real value[])
{
  return fmi2Error;
}

FMI2_Export fmi2Status fmi2CancelStep(fmi2Component c)
{
  return fmi2OK;
}

FMI2_Export fmi2Status fmi2GetStatus(fmi2Component c, const fmi2StatusKind s, fmi2Status* value)
{
  return fmi2Discard;
}

FMI2_Export fmi2Status fmi2GetRealStatus(fmi2Component c, const fmi2StatusKind s, fmi2Real* value)
{
  return fmi2Discard;
}

FMI2_Export fmi2Status fmi2GetIntegerStatus(fmi2Component c, const fmi2StatusKind s, fmi2Integer* value)
{
  return fmi2Discard;
}

FMI2_Export fmi2Status fmi2GetBooleanStatus(fmi2Component c, const fmi2StatusKind s, fmi2Boolean* value)
{
  return fmi2Discard;
}

FMI2_Export fmi2Status fmi2GetStringStatus(fmi2Component c, const fmi2StatusKind s, fmi2String* value)
{
  return fmi2Discard;
}
}
