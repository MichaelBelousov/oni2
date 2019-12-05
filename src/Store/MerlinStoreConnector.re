/*
 * MerlinStoreConnector.re
 *
 * This module connects merlin to the Store,
 * for some initial, basic language integration
 */

module Core = Oni_Core;
module Extensions = Oni_Extensions;
module Model = Oni_Model;

module Log = Core.Log;
module Zed_utf8 = Core.ZedBundled;

open Oni_Merlin;

let diagnosticsDebounceTime = Revery.Time.ms(300);

let runOnMainThread = (cb, arg) => {
  Revery.App.runOnMainThread(cb(arg));
};

let effectIfMerlinEnabled = effect => {
  let ret = (configuration: Core.Configuration.t) => {
    let isMerlinEnabled =
      Core.Configuration.getValue(c => c.experimentalMerlin, configuration);

    if (isMerlinEnabled) {effect} else {Isolinear.Effect.none};
  };

  ret;
};

let start = () => {
  let (stream, dispatch) = Isolinear.Stream.create();

  let pendingGetErrorsRequest = ref(None);

  let modelChangedEffect = (buffers: Model.Buffers.t, bu: Core.BufferUpdate.t) =>
    effectIfMerlinEnabled(
      Isolinear.Effect.create(~name="exthost.bufferUpdate", () =>
        switch (Model.Buffers.getBuffer(bu.id, buffers)) {
        | None => ()
        | Some(v) =>
          let lines = Core.Buffer.getLines(v);
          let fileType = Core.Buffer.getFileType(v);
          let uri = Core.Buffer.getUri(v);
          switch (fileType, Core.Buffer.getFilePath(v)) {
          | (Some(ft), Some(path)) when ft == "reason" || ft == "ocaml" =>
            let cb = err => {
              let modelDiagnostics =
                MerlinProtocolConverter.toModelDiagnostics(err);
              Log.info(
                "Got "
                ++ string_of_int(List.length(modelDiagnostics))
                ++ " errors",
              );
              Revery.App.runOnMainThread(() => {
                dispatch(
                  Model.Actions.DiagnosticsSet(
                    uri,
                    "merlin",
                    modelDiagnostics,
                  ),
                )
              });
            };

            switch (pendingGetErrorsRequest^) {
            | None => ()
            | Some(p) => p()
            };

            pendingGetErrorsRequest :=
              Some(
                Revery.Tick.timeout(
                  () => {
                    MerlinRequestQueue.getErrors(
                      Sys.getcwd(),
                      path,
                      lines,
                      cb,
                    )
                  },
                  diagnosticsDebounceTime,
                ),
              );
          | _ => ()
          };
        }
      ),
    );

  let checkCompletionsEffect = (state, meet: Model.Actions.completionMeet) =>
    effectIfMerlinEnabled(
      Isolinear.Effect.create(~name="merlin.checkCompletions", () => {
        switch (Model.Selectors.getActiveBuffer(state)) {
        | None => ()
        | Some(buf) =>
          let id = Core.Buffer.getId(buf);
          let lines = Core.Buffer.getLines(buf);
          let fileType = Core.Buffer.getFileType(buf);
          switch (fileType, Core.Buffer.getFilePath(buf)) {
          | (Some(ft), Some(path)) when ft == "reason" || ft == "ocaml" =>
            let cb = completions => {
              let modelCompletions =
                MerlinProtocolConverter.toModelCompletions(completions);
              dispatch(CompletionAddItems(meet, modelCompletions));
              ();
              // TODO: Show completion UI
            };
            open Oni_Core.Types;
            let cursorLine = meet.completionMeetLine |> Index.toInt0;
            let position = meet.completionMeetColumn |> Index.toInt0;

            if (cursorLine < Array.length(lines)
                && id == meet.completionMeetBufferId) {
              let _ =
                MerlinRequestQueue.getCompletions(
                  Sys.getcwd(),
                  path,
                  lines,
                  lines[cursorLine],
                  Core.Types.Position.ofInt0(cursorLine, position),
                  cb,
                );
              ();
            };

          | _ => ()
          };
        }
      }),
    );

  let updater = (state: Model.State.t, action) => {
    switch (action) {
    | Model.Actions.BufferUpdate(bu) => (
        state,
        modelChangedEffect(state.buffers, bu, state.configuration),
      )
    | Model.Actions.CompletionStart(completionMeet) => (
        state,
        checkCompletionsEffect(state, completionMeet, state.configuration),
      )
    | _ => (state, Isolinear.Effect.none)
    };
  };

  (updater, stream);
};
