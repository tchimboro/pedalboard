/*
 * pedalboard
 * Copyright 2021 Spotify AB
 *
 * Licensed under the GNU Public License, Version 3.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    https://www.gnu.org/licenses/gpl-3.0.html
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <mutex>
#include <optional>

#include "JuceHeader.h"
#if JUCE_LINUX
#include <sys/utsname.h>
#endif

#include "Plugin.h"
#include <pybind11/stl.h>

namespace Pedalboard {

// JUCE external plugins use some global state; here we lock that state
// to play nicely with the Python interpreter.
static std::mutex EXTERNAL_PLUGIN_MUTEX;
static int NUM_ACTIVE_EXTERNAL_PLUGINS = 0;

inline std::vector<std::string> findInstalledVSTPluginPaths() {
  // Ensure we have a MessageManager, which is required by the VST wrapper
  // Without this, we get an assert(false) from JUCE at runtime
  juce::MessageManager::getInstance();
  juce::VST3PluginFormat format;
  std::vector<std::string> pluginPaths;
  for (juce::String pluginIdentifier : format.searchPathsForPlugins(
           format.getDefaultLocationsToSearch(), true, false)) {
    pluginPaths.push_back(
        format.getNameOfPluginFromIdentifier(pluginIdentifier).toStdString());
  }
  return pluginPaths;
}

/**
 * The VST3 and Audio Unit format managers differ in how they look up plugins
 * that are already installed on the current machine. This approach allows us to
 * return file paths from both.
 */
#if JUCE_PLUGINHOST_AU && JUCE_MAC
class AudioUnitPathFinder {
public:
  static std::vector<std::string> findInstalledAudioUnitPaths() {
    // Ensure we have a MessageManager, which is required by the VST wrapper
    // Without this, we get an assert(false) from JUCE at runtime
    juce::MessageManager::getInstance();

    juce::AudioUnitPluginFormat format;

    std::vector<std::string> pluginPaths;
    for (juce::String pluginPath : searchPathsForPlugins(
             juce::FileSearchPath(
                 "/Library/Audio/Plug-Ins/Components;~/Library/"
                 "Audio/Plug-Ins/Components"),
             true, format)) {
      pluginPaths.push_back(pluginPath.toStdString());
    }
    return pluginPaths;
  }

private:
  static juce::StringArray
  searchPathsForPlugins(const juce::FileSearchPath &directoriesToSearch,
                        const bool recursive,
                        juce::AudioUnitPluginFormat &format) {
    juce::StringArray results;

    for (int i = 0; i < directoriesToSearch.getNumPaths(); ++i) {
      recursiveFileSearch(results, directoriesToSearch[i], recursive, format);
    }

    return results;
  }

  static void recursiveFileSearch(juce::StringArray &results,
                                  const juce::File &directory,
                                  const bool recursive,
                                  juce::AudioUnitPluginFormat &format) {
    for (const auto &iter : juce::RangedDirectoryIterator(
             directory, false, "*", juce::File::findFilesAndDirectories)) {
      auto f = iter.getFile();
      bool isPlugin = false;

      if (format.fileMightContainThisPluginType(f.getFullPathName())) {
        isPlugin = true;
        results.add(f.getFullPathName());
      }

      if (recursive && (!isPlugin) && f.isDirectory())
        recursiveFileSearch(results, f, true, format);
    }
  }
};
#endif

template <typename ExternalPluginType> class ExternalPlugin : public Plugin {
public:
  ExternalPlugin(std::string &_pathToPluginFile)
      : pathToPluginFile(_pathToPluginFile) {
    py::gil_scoped_release release;
    // Ensure we have a MessageManager, which is required by the VST wrapper
    // Without this, we get an assert(false) from JUCE at runtime
    juce::MessageManager::getInstance();

    juce::KnownPluginList pluginList;

    juce::OwnedArray<juce::PluginDescription> typesFound;
    ExternalPluginType format;

    juce::String pluginLoadError =
        "Plugin not found or not in the appropriate format.";
    pluginFormatManager.addDefaultFormats();

    auto pluginFileStripped =
        pathToPluginFile.trimCharactersAtEnd(juce::File::getSeparatorString());
    auto fileExists =
        juce::File::createFileWithoutCheckingPath(pluginFileStripped).exists();

    if (!fileExists) {
      throw pybind11::import_error("Unable to load plugin " +
                                   pathToPluginFile.toStdString() +
                                   ": plugin file not found.");
    }

    pluginList.scanAndAddFile(pluginFileStripped, false, typesFound, format);

    if (!typesFound.isEmpty()) {
      foundPluginDescription = *typesFound[0];
      reinstantiatePlugin();
    } else {
#if JUCE_LINUX
      auto machineName = []() -> juce::String {
        struct utsname unameData;
        auto res = uname(&unameData);

        if (res != 0)
          return {};

        return unameData.machine;
      }();

      juce::File pluginBundle(pluginFileStripped);

      auto pathToSharedObjectFile =
          pluginBundle.getChildFile("Contents")
              .getChildFile(machineName + "-linux")
              .getChildFile(pluginBundle.getFileNameWithoutExtension() + ".so");

      throw pybind11::import_error(
          "Unable to load plugin " + pathToPluginFile.toStdString() +
          ": unsupported plugin format or load failure. Plugin files or " +
          "shared library dependencies may be missing. (Try running `ldd \"" +
          pathToSharedObjectFile.getFullPathName().toStdString() + "\"` to " +
          "see which dependencies might be missing.).");
#else
      throw pybind11::import_error(
          "Unable to load plugin " + pathToPluginFile.toStdString() +
          ": unsupported plugin format or load failure.");
#endif
    }
  }

  ~ExternalPlugin() {
    {
      std::lock_guard<std::mutex> lock(EXTERNAL_PLUGIN_MUTEX);
      pluginInstance.reset();
      NUM_ACTIVE_EXTERNAL_PLUGINS--;

      if (NUM_ACTIVE_EXTERNAL_PLUGINS == 0) {
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
      }
    }
  }

  void reinstantiatePlugin() {
    // If we have an existing plugin, save its state and reload its state later:
    juce::MemoryBlock savedState;
    if (pluginInstance) {
      pluginInstance->getStateInformation(savedState);

      {
        std::lock_guard<std::mutex> lock(EXTERNAL_PLUGIN_MUTEX);
        // Delete the plugin instance itself:
        pluginInstance.reset();
        NUM_ACTIVE_EXTERNAL_PLUGINS--;
      }
    }

    juce::String loadError;
    {
      std::lock_guard<std::mutex> lock(EXTERNAL_PLUGIN_MUTEX);
      pluginInstance = pluginFormatManager.createPluginInstance(
          foundPluginDescription, ExternalLoadSampleRate,
          ExternalLoadMaximumBlockSize, loadError);

      if (!pluginInstance) {
        throw pybind11::import_error("Unable to load plugin " +
                                     pathToPluginFile.toStdString() + ": " +
                                     loadError.toStdString());
      }

      NUM_ACTIVE_EXTERNAL_PLUGINS++;
    }

    pluginInstance->setStateInformation(savedState.getData(),
                                        savedState.getSize());
    pluginInstance->reset();
  }

  void setNumChannels(int numChannels) {
    if (!pluginInstance)
      return;

    pluginInstance->disableNonMainBuses();

    auto mainInputBus = pluginInstance->getBus(true, 0);
    auto mainOutputBus = pluginInstance->getBus(false, 0);

    if (!mainInputBus) {
      throw std::invalid_argument(
          "Plugin '" + pluginInstance->getName().toStdString() +
          "' does not accept audio input. It may be an instrument plug-in "
          "and not an audio effect processor.");
    }

    // Disable all other input buses to avoid crashing:
    for (int i = 1; i < pluginInstance->getBusCount(true); i++) {
      pluginInstance->getBus(true, i)->enable(false);
    }

    // ...and all other output buses too:
    for (int i = 1; i < pluginInstance->getBusCount(false); i++) {
      pluginInstance->getBus(false, i)->enable(false);
    }

    // Try to change the input and output bus channel counts...
    mainInputBus->setNumberOfChannels(numChannels);
    mainOutputBus->setNumberOfChannels(numChannels);

    // If, post-reload, we still can't use the right number of channels, let's
    // conclude the plugin doesn't allow this channel count.
    if (mainInputBus->getNumberOfChannels() != numChannels ||
        mainOutputBus->getNumberOfChannels() != numChannels) {

      throw std::invalid_argument(
          "Plugin '" + pluginInstance->getName().toStdString() +
          "' does not support " + std::to_string(numChannels) +
          "-channel input and output. (Main bus currently expects " +
          std::to_string(mainInputBus->getNumberOfChannels()) +
          " input channels and " +
          std::to_string(mainOutputBus->getNumberOfChannels()) +
          " output channels.)");
    }
  }

  const juce::String getName() const {
    return pluginInstance ? pluginInstance->getName() : "<unknown>";
  }

  const int getNumChannels() const {
    // Input and output channel counts should match.
    if (!pluginInstance) {
      return 0;
    }

    auto mainInputBus = pluginInstance->getBus(true, 0);
    if (!mainInputBus) {
      return 0;
    }

    return mainInputBus->getNumberOfChannels();
  }

  void prepare(const juce::dsp::ProcessSpec &spec) override {
    if (pluginInstance) {
      setNumChannels(spec.numChannels);
      pluginInstance->setRateAndBufferSizeDetails(spec.sampleRate,
                                                  spec.maximumBlockSize);
      pluginInstance->prepareToPlay(spec.sampleRate, spec.maximumBlockSize);
      pluginInstance->setNonRealtime(true);
    }
  }

  void reset() noexcept override {
    if (pluginInstance) {
      // Some VSTs don't actually clear their internal state when calling
      // reset(). Here, we force a reset by reinstantiating the plugin.
      pluginInstance->reset();
      reinstantiatePlugin();
    }
  }

  void
  process(const juce::dsp::ProcessContextReplacing<float> &context) override {
    if (pluginInstance) {
      juce::MidiBuffer emptyMidiBuffer;
      if (context.usesSeparateInputAndOutputBlocks()) {
        throw std::runtime_error("Not implemented yet - "
                                 "no support for using separate "
                                 "input and output blocks.");
      } else {
        size_t pluginBufferChannelCount = 0;
        // Iterate through all input busses and add their input channels to our
        // buffer:
        for (size_t i = 0;
             i < static_cast<size_t>(pluginInstance->getBusCount(true)); i++) {
          if (pluginInstance->getBus(true, i)->isEnabled()) {
            pluginBufferChannelCount +=
                pluginInstance->getBus(true, i)->getNumberOfChannels();
          }
        }

        juce::dsp::AudioBlock<float> &outputBlock = context.getOutputBlock();

        std::vector<float *> channelPointers(pluginBufferChannelCount);

        for (size_t i = 0; i < outputBlock.getNumChannels(); i++) {
          channelPointers[i] = outputBlock.getChannelPointer(i);
        }

        // Depending on the bus layout, we may have to pass extra buffers to the
        // plugin that we don't use. Use vector here to ensure the memory is
        // freed via RAII.
        std::vector<std::vector<float>> dummyChannels;
        for (size_t i = outputBlock.getNumChannels();
             i < pluginBufferChannelCount; i++) {
          std::vector<float> dummyChannel(outputBlock.getNumSamples());
          channelPointers[i] = dummyChannel.data();
          dummyChannels.push_back(dummyChannel);
        }

        if ((size_t)pluginInstance->getMainBusNumInputChannels() !=
            outputBlock.getNumChannels()) {
          throw std::invalid_argument(
              "Plugin '" + pluginInstance->getName().toStdString() +
              "' was instantiated with " +
              std::to_string(pluginInstance->getMainBusNumInputChannels()) +
              "-channel input, but data provided was " +
              std::to_string(outputBlock.getNumChannels()) + "-channel.");
        }

        if ((size_t)pluginInstance->getMainBusNumOutputChannels() <
            outputBlock.getNumChannels()) {
          throw std::invalid_argument(
              "Plugin '" + pluginInstance->getName().toStdString() +
              "' produces " +
              std::to_string(pluginInstance->getMainBusNumOutputChannels()) +
              "-channel output, but data provided was " +
              std::to_string(outputBlock.getNumChannels()) +
              "-channel. (The number of channels returned must match the "
              "number of channels passed in.)");
        }

        // Create an audio buffer that doesn't actually allocate anything, but
        // just points to the data in the ProcessContext.
        juce::AudioBuffer<float> audioBuffer(channelPointers.data(),
                                             pluginBufferChannelCount,
                                             outputBlock.getNumSamples());
        pluginInstance->processBlock(audioBuffer, emptyMidiBuffer);
      }
    }
  }

  std::vector<juce::AudioProcessorParameter *> getParameters() const {
    std::vector<juce::AudioProcessorParameter *> parameters;
    for (auto *parameter : pluginInstance->getParameters()) {
      parameters.push_back(parameter);
    }
    return parameters;
  }

  juce::AudioProcessorParameter *getParameter(const std::string &name) const {
    for (auto *parameter : pluginInstance->getParameters()) {
      if (parameter->getName(512).toStdString() == name) {
        return parameter;
      }
    }
    return nullptr;
  }

private:
  constexpr static int ExternalLoadSampleRate = 44100,
                       ExternalLoadMaximumBlockSize = 8192;
  juce::String pathToPluginFile;
  juce::PluginDescription foundPluginDescription;
  juce::AudioPluginFormatManager pluginFormatManager;
  std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
};

inline void init_external_plugins(py::module &m) {
  py::class_<juce::AudioProcessorParameter>(
      m, "_AudioProcessorParameter",
      "An abstract base class for parameter objects that can be added to an "
      "AudioProcessor.")
      .def("__repr__",
           [](juce::AudioProcessorParameter &parameter) {
             std::ostringstream ss;
             ss << "<pedalboard.AudioProcessorParameter";
             ss << " name=\"" << parameter.getName(512).toStdString() << "\"";
             if (!parameter.getLabel().isEmpty())
               ss << " label=\"" << parameter.getLabel().toStdString() << "\"";
             if (parameter.isBoolean())
               ss << " boolean";
             if (parameter.isDiscrete())
               ss << " discrete";
             ss << " raw_value=" << parameter.getValue();
             ss << ">";
             return ss.str();
           })
      .def_property(
          "raw_value", &juce::AudioProcessorParameter::getValue,
          &juce::AudioProcessorParameter::setValue,
          "The internal value of this parameter. Convention is that this "
          "parameter should be between 0 and 1.0. This may or may not "
          "correspond with the value shown to the user.")
      .def_property_readonly(
          "default_raw_value", &juce::AudioProcessorParameter::getDefaultValue,
          "The default internal value of this parameter. Convention is that "
          "this parameter should be between 0 and 1.0. This may or may not "
          "correspond with the value shown to the user.")
      .def(
          "get_name",
          [](juce::AudioProcessorParameter &param, int length) {
            return param.getName(length).toStdString();
          },
          py::arg("maximum_string_length"),
          "Returns the name to display for this parameter, which is made to "
          "fit within the given string length")
      .def_property_readonly(
          "name",
          [](juce::AudioProcessorParameter &param) {
            return param.getName(512).toStdString();
          },
          "Returns the name to display for this parameter at its longest.")
      .def_property_readonly(
          "label",
          [](juce::AudioProcessorParameter &param) {
            return param.getLabel().toStdString();
          },
          "Some parameters may be able to return a label string for their "
          "units. For example \"Hz\" or \"%\".")
      .def_property_readonly(
          "num_steps", &juce::AudioProcessorParameter::getNumSteps,
          "Returns the number of steps that this parameter's range should be "
          "quantised into. See also: is_discrete, is_boolean.")
      .def_property_readonly("is_discrete",
                             &juce::AudioProcessorParameter::isDiscrete,
                             "Returns whether the parameter uses discrete "
                             "values, based on the result of getNumSteps, or "
                             "allows the host to select values continuously.")
      .def_property_readonly(
          "is_boolean", &juce::AudioProcessorParameter::isBoolean,
          "Returns whether the parameter represents a boolean switch, "
          "typically with \"On\" and \"Off\" states.")
      .def(
          "get_text_for_raw_value",
          [](juce::AudioProcessorParameter &param, float value,
             int maximumStringLength) {
            return param.getText(value, maximumStringLength).toStdString();
          },
          py::arg("raw_value"), py::arg("maximum_string_length") = 512,
          "Returns a textual version of the supplied normalised parameter "
          "value.")
      .def(
          "get_raw_value_for_text",
          [](juce::AudioProcessorParameter &param, std::string &text) {
            return param.getValueForText(text);
          },
          py::arg("string_value"),
          "Returns the raw value of the supplied text. Plugins may handle "
          "errors however they see fit, but will likely not raise exceptions.")
      .def_property_readonly(
          "is_orientation_inverted",
          &juce::AudioProcessorParameter::isOrientationInverted,
          "If true, this parameter operates in the reverse direction. (Not "
          "all plugin formats will actually use this information).")
      .def_property_readonly("is_automatable",
                             &juce::AudioProcessorParameter::isAutomatable,
                             "Returns true if this parameter can be automated.")
      .def_property_readonly(
          "is_automatable", &juce::AudioProcessorParameter::isAutomatable,
          "Returns true if this parameter can be automated (i.e.: scheduled to "
          "change over time, in real-time, in a DAW).")
      .def_property_readonly(
          "is_meta_parameter", &juce::AudioProcessorParameter::isMetaParameter,
          "A meta-parameter is a parameter that changes other parameters.")
      .def_property_readonly(
          "index", &juce::AudioProcessorParameter::getParameterIndex,
          "The index of this parameter in its plugin's parameter list.")
      .def_property_readonly(
          "string_value",
          [](juce::AudioProcessorParameter &param) {
            return param.getCurrentValueAsText().toStdString();
          },
          "Returns the current value of the parameter as a string.");

#if JUCE_PLUGINHOST_VST3 && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
  py::class_<ExternalPlugin<juce::VST3PluginFormat>, Plugin>(
      m, "_VST3Plugin",
      "A wrapper around any Steinberg® VST3 audio effect plugin. Note that "
      "plugins must already support the operating system currently in use "
      "(i.e.: if you're running Linux but trying to open a VST that does not "
      "support Linux, this will fail).",
      py::dynamic_attr())
      .def(py::init([](std::string &pathToPluginFile) {
             return new ExternalPlugin<juce::VST3PluginFormat>(
                 pathToPluginFile);
           }),
           py::arg("path_to_plugin_file"))
      .def("__repr__",
           [](ExternalPlugin<juce::VST3PluginFormat> &plugin) {
             std::ostringstream ss;
             ss << "<pedalboard.VST3Plugin";
             ss << " \"" << plugin.getName() << "\"";
             ss << " at " << &plugin;
             ss << ">";
             return ss.str();
           })
      .def_property_readonly_static(
          "installed_plugins",
          [](py::object /* cls */) { return findInstalledVSTPluginPaths(); },
          "Return a list of paths to VST3 plugins installed in the default "
          "location on this system. This list may not be exhaustive, and "
          "plugins in this list are not guaranteed to be compatible with "
          "Pedalboard.")
      .def_property_readonly(
          "_parameters", &ExternalPlugin<juce::VST3PluginFormat>::getParameters,
          py::return_value_policy::reference_internal)
      .def("_get_parameter",
           &ExternalPlugin<juce::VST3PluginFormat>::getParameter,
           py::return_value_policy::reference_internal);
#endif

#if JUCE_PLUGINHOST_AU && JUCE_MAC
  py::class_<ExternalPlugin<juce::AudioUnitPluginFormat>, Plugin>(
      m, "_AudioUnitPlugin",
      "A wrapper around any Apple Audio Unit audio effect plugin. Only "
      "available on macOS.",
      py::dynamic_attr())
      .def(py::init([](std::string &pathToPluginFile) {
             return new ExternalPlugin<juce::AudioUnitPluginFormat>(
                 pathToPluginFile);
           }),
           py::arg("path_to_plugin_file"))
      .def("__repr__",
           [](const ExternalPlugin<juce::AudioUnitPluginFormat> &plugin) {
             std::ostringstream ss;
             ss << "<pedalboard.AudioUnitPlugin";
             ss << " \"" << plugin.getName() << "\"";
             ss << " at " << &plugin;
             ss << ">";
             return ss.str();
           })
      .def_property_readonly_static(
          "installed_plugins",
          [](py::object /* cls */) {
            return AudioUnitPathFinder::findInstalledAudioUnitPaths();
          },
          "Return a list of paths to Audio Units installed in the default "
          "location on this system. This list may not be exhaustive, and "
          "plugins in this list are not guaranteed to be compatible with "
          "Pedalboard.")
      .def_property_readonly(
          "_parameters",
          &ExternalPlugin<juce::AudioUnitPluginFormat>::getParameters,
          py::return_value_policy::reference_internal)
      .def("_get_parameter",
           &ExternalPlugin<juce::AudioUnitPluginFormat>::getParameter,
           py::return_value_policy::reference_internal);
#endif
}

} // namespace Pedalboard