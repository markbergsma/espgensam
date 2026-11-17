#include "../components/gensam/sam_runtime.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace esphome::gensam;

static std::string replace(std::string text, const std::string &from, const std::string &to) {
  const auto pos = text.find(from);
  assert(pos != std::string::npos);
  text.replace(pos, from.size(), to);
  return text;
}

int main(int argc, char **argv) {
  const std::string sam =
      "Magic: GLM5_v2\nHeader_End:\nModel:8351B\nSerial:123\nName:Left\nXPos:1\nYPos:5\nZPos:5\n"
      "Data_End:\nGroup_Name: Studio\nGroup_Crossover:90\nGroup_Nodes:1\n"
      "Class:SubwooferGen2\nSerial:123\n"
      "Group_ON:1\nInput:3\nLFE_Channel:2\nLFE_+10:1\n"
      "LFE_Level:-2\nLevel_Sensitivity:-2.5\nGroup_Sensitivity:-1\nPhase(degrees):-165\n"
      "Notch:1\nFrequency:56.2\nGain:-6\nQ_Value:4\nSR:12000\nData_End:\nGroup_End:\n";
  SamPresets presets;
  std::string error;
  std::vector<std::string> warnings;
  assert(parse_sam_runtime(sam, presets, warnings, error));
  assert(presets.groups().size() == 1);
  const auto utf8_name = replace(sam, "Group_Name: Studio", "Group_Name: L\xc3\xa4hikentt\xc3\xa4");
  assert(parse_sam_runtime(utf8_name, presets, warnings, error));
  assert(std::string(presets.groups()[0].name) == "L\xc3\xa4hikentt\xc3\xa4");
  const auto latin1_name = replace(sam, "Group_Name: Studio", "Group_Name: L\xe4hikentt\xe4");
  assert(parse_sam_runtime(latin1_name, presets, warnings, error));
  assert(std::string(presets.groups()[0].name) == "L\xc3\xa4hikentt\xc3\xa4");
  const auto unicode_name = replace(sam, "Group_Name: Studio",
                                    "Group_Name: \xe6\x97\xa5\xf0\x9f\x8e\xb5");
  assert(parse_sam_runtime(unicode_name, presets, warnings, error));
  assert(std::string(presets.groups()[0].name) == "\xe6\x97\xa5\xf0\x9f\x8e\xb5");
  const auto invalid_utf8 = replace(sam, "Group_Name: Studio", "Group_Name: \xed\xa0\x80");
  assert(parse_sam_runtime(invalid_utf8, presets, warnings, error));
  assert(std::string(presets.groups()[0].name) == "\xc3\xad\xc2\xa0\xc2\x80");
  assert(parse_sam_runtime(sam, presets, warnings, error));
  const auto &device = presets.groups()[0].devices[0];
  assert(device.unique_id == 123 && device.source == SOURCE_DIGITAL_AES3);
  assert(presets.owned[0].layouts[0].name == "Left");
  assert(presets.device_names().at(123) == "Left");
  assert(device.aes3_channel == AES3_CHANNEL_A && device.lfe_channel == AES3_CHANNEL_B);
  assert(device.lfe_level_db == 8 && device.level_db == -3.5f && device.delay_samples == 289);
  assert(device.band_count == 20 && device.bands[0].type == PeqType::PEAKING);
  assert(std::fabs(device.bands[0].frequency_hz - 56.2f) < 0.001f);
  assert(!parse_sam_runtime(sam.substr(0, sam.size() - 11), presets, warnings, error));
  assert(presets.groups().size() == 1 && presets.groups()[0].devices[0].level_db == -3.5f);
  assert(!parse_sam_runtime(sam + sam, presets, warnings, error));
  assert(!parse_sam_runtime(std::string(65537, 'x'), presets, warnings, error));
  assert(parse_sam_runtime(sam + std::string(SAM_MAX_BYTES - sam.size(), '-'),
                           presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "SR:12000", "SR:48000"), presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Input:3", "Input:9"), presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "LFE_Channel:2", "LFE_Channel:3"), presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Level_Sensitivity:-2.5", "Level_Sensitivity:-100"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Class:SubwooferGen2\nSerial:123\n",
                                   "Class:SubwooferGen2\nSerial:123\nNotch:1\nFrequency:50\n"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Class:SubwooferGen2", "Class:Unknown"),
                            presets, warnings, error));
  auto threeway = replace(sam, "Class:SubwooferGen2", "Class:ThreewayGen2");
  threeway = replace(threeway, "SR:12000", "SR:96000");
  assert(parse_sam_runtime(threeway, presets, warnings, error));
  assert(presets.groups()[0].devices[0].peq_design_rate_hz == 96000);
  assert(!parse_sam_runtime(replace(threeway, "SR:96000", "SR:48000"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Frequency:56.2", "Frequency:6000"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Q_Value:4", "Q_Value:0"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Gain:-6", "Gain:1e30"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "SR:12000", "SR:12000.5"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Frequency:56.2", "Frequency:nan"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Group_Name: Studio", "Group_Name:"),
                            presets, warnings, error));
  assert(!parse_sam_runtime(replace(sam, "Group_ON:1", "Group_ON:invalid"),
                            presets, warnings, error));
  const auto no_crossover = replace(sam, "Group_Crossover:90\n", "");
  assert(parse_sam_runtime(no_crossover, presets, warnings, error));
  assert(presets.groups()[0].devices[0].delay_samples == 0);
  assert(!warnings.empty());
  assert(parse_sam_runtime(sam, presets, warnings, error));
  assert(presets.groups()[0].devices[0].unique_id == 123);
  const std::string other_group =
      "Group_Name:Other\nClass:SubwooferGen2\nSerial:999\nData_End:\nGroup_End:\n";
  assert(parse_sam_runtime(sam + other_group, presets, warnings, error));
  assert(presets.groups().size() == 2 && presets.groups()[1].devices[0].unique_id == 999);
  assert(parse_sam_runtime(replace(sam, "Level_Sensitivity:-2.5", "Level_Sensitivity:-999"),
                           presets, warnings, error));
  assert(!warnings.empty() && presets.groups()[0].devices[0].level_db == -1);
  assert(parse_sam_runtime(replace(sam, "Crossover:90", "Crossover:1"), presets, warnings, error));
  assert(presets.groups()[0].devices[0].delay_samples == 0 && !warnings.empty());
  auto twoway = replace(sam, "Class:SubwooferGen2", "Class:TwowayGen2");
  twoway = replace(twoway, "SR:12000", "SR:48000");
  twoway = replace(twoway, "Notch:1", "LP_Shelve:1\nFrequency:80\nGain:-1\nSR:48000\n"
                                      "HP_Shelve:1\nFrequency:8000\nGain:-1\nSR:48000\nNotch:1");
  assert(parse_sam_runtime(twoway, presets, warnings, error));
  const auto &bands = presets.groups()[0].devices[0];
  assert(bands.bands[0].type == PeqType::LOW_SHELF);
  assert(bands.bands[2].type == PeqType::HIGH_SHELF);
  assert(bands.bands[4].type == PeqType::PEAKING);
  assert(bands.bands[1].type == PeqType::BYPASS);
  assert(!parse_sam_runtime(replace(twoway, "LP_Shelve:1", "LP_Shelve:3"),
                            presets, warnings, error));
  auto many_groups = std::string("Magic: GLM5_v2\nHeader_End:\n");
  for (size_t i = 0; i < SAM_MAX_GROUPS; ++i) {
    many_groups += "Group_Name:Group " + std::to_string(i) +
                   "\nClass:SubwooferGen2\nSerial:123\nData_End:\nGroup_End:\n";
  }
  assert(parse_sam_runtime(many_groups, presets, warnings, error));
  assert(presets.groups().size() == SAM_MAX_GROUPS);
  assert(!parse_sam_runtime(many_groups + "Group_Name:Overflow\n", presets, warnings, error));
  auto many_devices = std::string("Magic: GLM5_v2\nHeader_End:\nGroup_Name:Large group\n");
  for (size_t i = 1; i <= SAM_MAX_DEVICE_CONFIGS; ++i) {
    many_devices += "Class:SubwooferGen2\nSerial:" + std::to_string(i) + "\nData_End:\n";
  }
  assert(parse_sam_runtime(many_devices + "Group_End:\n", presets, warnings, error));
  assert(presets.groups()[0].device_count == SAM_MAX_DEVICE_CONFIGS);
  assert(!parse_sam_runtime(many_devices + "Class:SubwooferGen2\nSerial:999\nData_End:\nGroup_End:\n",
                            presets, warnings, error));
  assert(presets.groups()[0].device_count == SAM_MAX_DEVICE_CONFIGS);
  if (argc >= 2) {
    std::ifstream file(argv[1], std::ios::binary);
    assert(file);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!parse_sam_runtime(text, presets, warnings, error)) {
      std::cerr << "Sample import failed: " << error << '\n';
      return 1;
    }
    if (argc == 3 && std::string(argv[2]) == "--dump") {
      std::cout << std::setprecision(9);
      for (const auto &group : presets.groups()) {
        std::cout << "group\t" << group.name << '\n';
        for (size_t i = 0; i < group.device_count; ++i) {
          const auto &d = group.devices[i];
          std::cout << d.unique_id << '\t' << d.enabled << '\t' << d.crossover_hz << '\t'
                    << unsigned(d.source) << '\t' << unsigned(d.aes3_channel) << '\t'
                    << unsigned(d.lfe_channel) << '\t' << d.lfe_level_db << '\t'
                    << d.level_db << '\t' << d.delay_samples;
          for (size_t b = 0; b < d.band_count; ++b) {
            const auto &f = d.bands[b];
            std::cout << '\t' << unsigned(f.type) << '\t' << f.frequency_hz << '\t'
                      << f.gain_db << '\t' << f.q;
          }
          std::cout << '\n';
        }
      }
    } else {
      std::cout << "Sample imported: " << presets.groups().size() << " groups; "
                << warnings.size() << " warnings\n";
    }
  }
}
