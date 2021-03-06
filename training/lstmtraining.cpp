///////////////////////////////////////////////////////////////////////
// File:        lstmtraining.cpp
// Description: Training program for LSTM-based networks.
// Author:      Ray Smith
// Created:     Fri May 03 11:05:06 PST 2013
//
// (C) Copyright 2013, Google Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
///////////////////////////////////////////////////////////////////////

#ifdef GOOGLE_TESSERACT
#include "base/commandlineflags.h"
#endif
#include "commontraining.h"
#include "lstmtester.h"
#include "lstmtrainer.h"
#include "params.h"
#include "strngs.h"
#include "tprintf.h"
#include "unicharset_training_utils.h"

INT_PARAM_FLAG(debug_interval, 0, "How often to display the alignment.");
STRING_PARAM_FLAG(net_spec, "", "Network specification");
INT_PARAM_FLAG(net_mode, 192, "Controls network behavior.");
INT_PARAM_FLAG(perfect_sample_delay, 0,
               "How many imperfect samples between perfect ones.");
DOUBLE_PARAM_FLAG(target_error_rate, 0.01, "Final error rate in percent.");
DOUBLE_PARAM_FLAG(weight_range, 0.1, "Range of initial random weights.");
DOUBLE_PARAM_FLAG(learning_rate, 10.0e-4, "Weight factor for new deltas.");
DOUBLE_PARAM_FLAG(momentum, 0.5, "Decay factor for repeating deltas.");
DOUBLE_PARAM_FLAG(adam_beta, 0.999, "Decay factor for repeating deltas.");
INT_PARAM_FLAG(max_image_MB, 6000, "Max memory to use for images.");
STRING_PARAM_FLAG(continue_from, "", "Existing model to extend");
STRING_PARAM_FLAG(model_output, "lstmtrain", "Basename for output models");
STRING_PARAM_FLAG(train_listfile, "",
                  "File listing training files in lstmf training format.");
STRING_PARAM_FLAG(eval_listfile, "",
                  "File listing eval files in lstmf training format.");
BOOL_PARAM_FLAG(stop_training, false,
               "Just convert the training model to a runtime model.");
BOOL_PARAM_FLAG(convert_to_int, false,
                "Convert the recognition model to an integer model.");
BOOL_PARAM_FLAG(sequential_training, false,
                "Use the training files sequentially instead of round-robin.");
INT_PARAM_FLAG(append_index, -1, "Index in continue_from Network at which to"
               " attach the new network defined by net_spec");
BOOL_PARAM_FLAG(debug_network, false,
                "Get info on distribution of weight values");
INT_PARAM_FLAG(max_iterations, 0, "If set, exit after this many iterations");
STRING_PARAM_FLAG(traineddata, "",
                  "Combined Dawgs/Unicharset/Recoder for language model");
STRING_PARAM_FLAG(old_traineddata, "",
                  "When changing the character set, this specifies the old"
                  " character set that is to be replaced");

// Number of training images to train between calls to MaintainCheckpoints.
const int kNumPagesPerBatch = 100;

// Apart from command-line flags, input is a collection of lstmf files, that
// were previously created using tesseract with the lstm.train config file.
// The program iterates over the inputs, feeding the data to the network,
// until the error rate reaches a specified target or max_iterations is reached.
int main(int argc, char **argv) {
  ParseArguments(&argc, &argv);
  // Purify the model name in case it is based on the network string.
  if (FLAGS_model_output.empty()) {
    tprintf("Must provide a --model_output!\n");
    return 1;
  }
  if (FLAGS_traineddata.empty()) {
    tprintf("Must provide a --traineddata see training wiki\n");
    return 1;
  }
  STRING model_output = FLAGS_model_output.c_str();
  for (int i = 0; i < model_output.length(); ++i) {
    if (model_output[i] == '[' || model_output[i] == ']')
      model_output[i] = '-';
    if (model_output[i] == '(' || model_output[i] == ')')
      model_output[i] = '_';
  }
  // Setup the trainer.
  STRING checkpoint_file = FLAGS_model_output.c_str();
  checkpoint_file += "_checkpoint";
  STRING checkpoint_bak = checkpoint_file + ".bak";
  tesseract::LSTMTrainer trainer(
      nullptr, nullptr, nullptr, nullptr, FLAGS_model_output.c_str(),
      checkpoint_file.c_str(), FLAGS_debug_interval,
      static_cast<inT64>(FLAGS_max_image_MB) * 1048576);
  trainer.InitCharSet(FLAGS_traineddata.c_str());

  // Reading something from an existing model doesn't require many flags,
  // so do it now and exit.
  if (FLAGS_stop_training || FLAGS_debug_network) {
    if (!trainer.TryLoadingCheckpoint(FLAGS_continue_from.c_str(), nullptr)) {
      tprintf("Failed to read continue from: %s\n",
              FLAGS_continue_from.c_str());
      return 1;
    }
    if (FLAGS_debug_network) {
      trainer.DebugNetwork();
    } else {
      if (FLAGS_convert_to_int) trainer.ConvertToInt();
      if (!trainer.SaveTraineddata(FLAGS_model_output.c_str())) {
        tprintf("Failed to write recognition model : %s\n",
                FLAGS_model_output.c_str());
      }
    }
    return 0;
  }

  // Get the list of files to process.
  if (FLAGS_train_listfile.empty()) {
    tprintf("Must supply a list of training filenames! --train_listfile\n");
    return 1;
  }
  GenericVector<STRING> filenames;
  if (!tesseract::LoadFileLinesToStrings(FLAGS_train_listfile.c_str(),
                                         &filenames)) {
    tprintf("Failed to load list of training filenames from %s\n",
            FLAGS_train_listfile.c_str());
    return 1;
  }

  // Checkpoints always take priority if they are available.
  if (trainer.TryLoadingCheckpoint(checkpoint_file.string(), nullptr) ||
      trainer.TryLoadingCheckpoint(checkpoint_bak.string(), nullptr)) {
    tprintf("Successfully restored trainer from %s\n",
            checkpoint_file.string());
  } else {
    if (!FLAGS_continue_from.empty()) {
      // Load a past model file to improve upon.
      if (!trainer.TryLoadingCheckpoint(FLAGS_continue_from.c_str(),
                                        FLAGS_append_index >= 0
                                            ? FLAGS_continue_from.c_str()
                                            : FLAGS_old_traineddata.c_str())) {
        tprintf("Failed to continue from: %s\n", FLAGS_continue_from.c_str());
        return 1;
      }
      tprintf("Continuing from %s\n", FLAGS_continue_from.c_str());
      trainer.InitIterations();
    }
    if (FLAGS_continue_from.empty() || FLAGS_append_index >= 0) {
      if (FLAGS_append_index >= 0) {
        tprintf("Appending a new network to an old one!!");
        if (FLAGS_continue_from.empty()) {
          tprintf("Must set --continue_from for appending!\n");
          return 1;
        }
      }
      // We are initializing from scratch.
      if (!trainer.InitNetwork(FLAGS_net_spec.c_str(), FLAGS_append_index,
                               FLAGS_net_mode, FLAGS_weight_range,
                               FLAGS_learning_rate, FLAGS_momentum,
                               FLAGS_adam_beta)) {
        tprintf("Failed to create network from spec: %s\n",
                FLAGS_net_spec.c_str());
        return 1;
      }
      trainer.set_perfect_delay(FLAGS_perfect_sample_delay);
    }
  }
  if (!trainer.LoadAllTrainingData(
          filenames, FLAGS_sequential_training ? tesseract::CS_SEQUENTIAL
                                               : tesseract::CS_ROUND_ROBIN)) {
    tprintf("Load of images failed!!\n");
    return 1;
  }

  tesseract::LSTMTester tester(static_cast<inT64>(FLAGS_max_image_MB) *
                               1048576);
  tesseract::TestCallback tester_callback = nullptr;
  if (!FLAGS_eval_listfile.empty()) {
    if (!tester.LoadAllEvalData(FLAGS_eval_listfile.c_str())) {
      tprintf("Failed to load eval data from: %s\n",
              FLAGS_eval_listfile.c_str());
      return 1;
    }
    tester_callback =
        NewPermanentTessCallback(&tester, &tesseract::LSTMTester::RunEvalAsync);
  }
  do {
    // Train a few.
    int iteration = trainer.training_iteration();
    for (int target_iteration = iteration + kNumPagesPerBatch;
         iteration < target_iteration;
         iteration = trainer.training_iteration()) {
      trainer.TrainOnLine(&trainer, false);
    }
    STRING log_str;
    trainer.MaintainCheckpoints(tester_callback, &log_str);
    tprintf("%s\n", log_str.string());
  } while (trainer.best_error_rate() > FLAGS_target_error_rate &&
           (trainer.training_iteration() < FLAGS_max_iterations ||
            FLAGS_max_iterations == 0));
  delete tester_callback;
  tprintf("Finished! Error rate = %g\n", trainer.best_error_rate());
  return 0;
} /* main */


