import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
} from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import { ProfileManager } from "../../web/src/components/ProfileManager";
import { OutputPriorityManager } from "../../web/src/components/OutputPriorityManager";
import { SplitManager } from "../../web/src/components/SplitManager";
import {
  OutputPriority,
  Request,
  Response,
} from "../../web/src/proto/zmk/ble_management/ble_management";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));

const wire = (response: Parameters<typeof Response.create>[0]) => ({
  custom: {
    call: { payload: Response.encode(Response.create(response)).finish() },
  },
});
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: Error) => void;
  const promise = new Promise<T>((done, fail) => {
    resolve = done;
    reject = fail;
  });
  return { promise, resolve, reject };
}
const app = () =>
  createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] });
const profiles = (name: string, isOpen = false) =>
  wire({
    getProfiles: {
      maxProfiles: 5,
      profiles: [
        {
          index: 0,
          name,
          isOpen,
          isActive: false,
          peerIdentity: isOpen
            ? undefined
            : {
                addressType: 0,
                address: new Uint8Array([6, 5, 4, 3, 2, 1]),
              },
        },
      ],
    },
  });

beforeEach(() => {
  (call_rpc as jest.Mock).mockReset();
  jest.spyOn(window, "confirm").mockReturnValue(true);
  jest.spyOn(console, "error").mockImplementation(() => {});
});
afterEach(() => jest.restoreAllMocks());

const panels = [
  {
    name: "profiles",
    Component: ProfileManager,
    reply: (first: boolean) => profiles(first ? "first-host" : "second-host"),
    marker: (first: boolean) =>
      screen.findByText(first ? "first-host" : "second-host"),
    oldMarker: () => screen.queryByText("first-host"),
  },
  {
    name: "output priority",
    Component: OutputPriorityManager,
    reply: (first: boolean) =>
      wire({
        getOutputPriority: {
          priority: first
            ? OutputPriority.OUTPUT_PRIORITY_USB
            : OutputPriority.OUTPUT_PRIORITY_BLE,
        },
      }),
    marker: (first: boolean) =>
      screen.findByText(first ? "USB" : "BLE", { selector: ".priority-badge" }),
    oldMarker: () => screen.queryByText("USB", { selector: ".priority-badge" }),
  },
  {
    name: "split",
    Component: SplitManager,
    reply: (first: boolean) =>
      wire({ getSplitInfo: { info: { isSplit: true, isCentral: first } } }),
    marker: (first: boolean) =>
      screen.findByText(first ? "Central" : "Peripheral"),
    oldMarker: () => screen.queryByText("Central"),
  },
];

describe.each(panels)("$name subsystem-index ownership", (spec) => {
  it.each(["success", "error"])(
    "resets the panel on one connection and retires the previous index's %s",
    async (outcome) => {
      const first = app();
      const second = {
        ...first,
        findSubsystem: () => ({
          index: 7,
          identifier: SUBSYSTEM_IDENTIFIER,
          uiUrl: [],
        }),
      };
      expect(second.state.connection).toBe(first.state.connection);
      const old = deferred<ReturnType<typeof wire>>();
      const fresh = deferred<ReturnType<typeof wire>>();
      let oldReads = 0;
      (call_rpc as jest.Mock).mockImplementation((_connection, raw) =>
        raw.custom.call.subsystemIndex === 0
          ? ++oldReads === 1
            ? Promise.resolve(spec.reply(true))
            : old.promise
          : fresh.promise,
      );
      const { Component } = spec;
      const view = render(
        <ZMKAppProvider value={first}>
          <Component />
        </ZMKAppProvider>,
      );
      await spec.marker(true);
      fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
      expect(oldReads).toBe(2);
      view.rerender(
        <ZMKAppProvider value={second}>
          <Component />
        </ZMKAppProvider>,
      );
      expect(spec.oldMarker()).not.toBeInTheDocument();
      await act(async () => fresh.resolve(spec.reply(false)));
      expect(await spec.marker(false)).toBeInTheDocument();
      await act(async () => {
        if (outcome === "success") old.resolve(spec.reply(true));
        else old.reject(new Error("retired index failed"));
      });
      expect(await spec.marker(false)).toBeInTheDocument();
      expect(
        screen.queryByText(/retired index failed/),
      ).not.toBeInTheDocument();
      expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
      expect(
        (call_rpc as jest.Mock).mock.calls.map(
          ([, raw]) => raw.custom.call.subsystemIndex,
        ),
      ).toEqual([0, 0, 7]);
    },
  );
});

const mutations = [
  {
    name: "unpair",
    isMutation: (request: Request) => !!request.unpairProfile,
    action: () =>
      fireEvent.click(screen.getByRole("button", { name: /Unpair/ })),
    success: () => wire({ unpairProfile: { success: true } }),
    refreshed: () => profiles("", true),
    assertRefreshed: () =>
      expect(
        screen.getByText("No device paired in this slot."),
      ).toBeInTheDocument(),
  },
  {
    name: "save name",
    isMutation: (request: Request) => !!request.setProfileName,
    action: () => {
      fireEvent.click(screen.getByTitle("Edit name"));
      fireEvent.change(screen.getByPlaceholderText("Device name"), {
        target: { value: "updated-host" },
      });
      fireEvent.click(screen.getByRole("button", { name: /Save/ }));
    },
    success: () => wire({ setProfileName: { success: true } }),
    refreshed: () => profiles("updated-host"),
    assertRefreshed: () =>
      expect(screen.getByText("updated-host")).toBeInTheDocument(),
  },
];

describe.each(mutations)("profile $name ownership", (spec) => {
  it.each(["success", "RPC error", "rejection"])(
    "retains same-connection loading and follow-up ownership on %s",
    async (outcome) => {
      const current = app();
      const mutation = deferred<ReturnType<typeof wire>>();
      const followUp = deferred<ReturnType<typeof wire>>();
      let reads = 0;
      let writes = 0;
      (call_rpc as jest.Mock).mockImplementation((_connection, raw) => {
        const request = Request.decode(raw.custom.call.payload);
        if (spec.isMutation(request)) {
          writes++;
          return mutation.promise;
        }
        expect(request.getProfiles).toBeDefined();
        return ++reads === 1
          ? Promise.resolve(profiles("first-host"))
          : followUp.promise;
      });
      render(
        <ZMKAppProvider value={current}>
          <ProfileManager />
        </ZMKAppProvider>,
      );
      await screen.findByText("first-host");
      spec.action();
      expect(writes).toBe(1);
      expect(reads).toBe(1);
      expect(screen.getByRole("button", { name: /Refresh/ })).toBeDisabled();
      await act(async () => {
        if (outcome === "success") mutation.resolve(spec.success());
        else if (outcome === "RPC error")
          mutation.resolve(
            wire({ error: { message: "current mutation denied" } }),
          );
        else mutation.reject(new Error("current mutation rejected"));
      });
      if (outcome === "success") {
        await waitFor(() => expect(reads).toBe(2));
        expect(screen.getByRole("button", { name: /Refresh/ })).toBeDisabled();
        await act(async () => followUp.resolve(spec.refreshed()));
        spec.assertRefreshed();
      } else {
        expect(reads).toBe(1);
        expect(screen.getByText(/current mutation/)).toBeInTheDocument();
      }
      expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
      for (const [connection] of (call_rpc as jest.Mock).mock.calls) {
        expect(connection).toBe(current.state.connection);
      }
    },
  );

  it.each(["success", "error"])(
    "retires the mutation %s and follow-up after connection replacement",
    async (outcome) => {
      const first = app();
      const second = app();
      const mutation = deferred<ReturnType<typeof wire>>();
      let oldReads = 0;
      let oldWrites = 0;
      (call_rpc as jest.Mock).mockImplementation((connection, raw) => {
        const request = Request.decode(raw.custom.call.payload);
        if (connection !== first.state.connection)
          return Promise.resolve(profiles("second-host"));
        if (spec.isMutation(request)) {
          oldWrites++;
          return mutation.promise;
        }
        oldReads++;
        return Promise.resolve(profiles("first-host"));
      });
      const view = render(
        <ZMKAppProvider value={first}>
          <ProfileManager />
        </ZMKAppProvider>,
      );
      await screen.findByText("first-host");
      spec.action();
      expect(oldWrites).toBe(1);
      view.rerender(
        <ZMKAppProvider value={second}>
          <ProfileManager />
        </ZMKAppProvider>,
      );
      await screen.findByText("second-host");
      expect(screen.queryByPlaceholderText("Device name")).toBeNull();
      await act(async () => {
        if (outcome === "success") mutation.resolve(spec.success());
        else mutation.reject(new Error("retired mutation failed"));
      });
      expect(screen.getByText("second-host")).toBeInTheDocument();
      expect(
        screen.queryByText(/retired mutation failed/),
      ).not.toBeInTheDocument();
      expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
      expect(oldReads).toBe(1);
      expect(oldWrites).toBe(1);
    },
  );
});
