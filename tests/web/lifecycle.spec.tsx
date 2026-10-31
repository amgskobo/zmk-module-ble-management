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
  Request,
  Response,
  OutputPriority,
} from "../../web/src/proto/zmk/ble_management/ble_management";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));

const wire = (response: Parameters<typeof Response.create>[0]) => ({
  custom: {
    call: { payload: Response.encode(Response.create(response)).finish() },
  },
});
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: Error) => void;
  const promise = new Promise<T>((done, fail) => {
    resolve = done;
    reject = fail;
  });
  return { promise, resolve, reject };
}
const cases = [
  {
    name: "profiles",
    Component: ProfileManager,
    read: (request: Request) => !!request.getProfiles,
    reply: (first: boolean) =>
      wire({
        getProfiles: {
          maxProfiles: 5,
          profiles: [
            {
              index: 0,
              name: first ? "first-host" : "second-host",
              isOpen: false,
              isActive: false,
            },
          ],
        },
      }),
    marker: (first: boolean) =>
      screen.findByText(first ? "first-host" : "second-host"),
    action: () =>
      fireEvent.click(
        screen.getByRole("button", { name: "Switch to this profile" }),
      ),
    success: () => wire({ switchProfile: { success: true } }),
  },
  {
    name: "output priority",
    Component: OutputPriorityManager,
    read: (request: Request) => !!request.getOutputPriority,
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
    action: () =>
      fireEvent.click(
        screen.getByRole("button", { name: /Bluetooth \(BLE\)/ }),
      ),
    success: () => wire({ setOutputPriority: { success: true } }),
  },
  {
    name: "split",
    Component: SplitManager,
    read: (request: Request) => !!request.getSplitInfo,
    reply: (first: boolean) =>
      wire({
        getSplitInfo: {
          info: {
            isSplit: true,
            isCentral: first,
            centralBonded: true,
            peripheralConnected: false,
          },
        },
      }),
    marker: (first: boolean) =>
      screen.findByText(first ? "Central" : "Peripheral"),
    action: () =>
      fireEvent.click(
        screen.getByRole("button", { name: /Clear All Host Pairings/ }),
      ),
    success: () => wire({ forgetSplitBond: { success: true } }),
  },
];

beforeEach(() => {
  jest.spyOn(window, "confirm").mockReturnValue(true);
  jest.spyOn(window, "alert").mockImplementation(() => {});
  jest.spyOn(console, "error").mockImplementation(() => {});
});
afterEach(() => jest.restoreAllMocks());

describe.each(cases)("$name connection ownership", (spec) => {
  const { Component } = spec;
  const apps = () => [
    createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] }),
    createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] }),
  ];
  const decode = (raw: { custom: { call: { payload: Uint8Array } } }) =>
    Request.decode(raw.custom.call.payload);

  it.each(["success", "error"])(
    "ignores an old load %s after the new source loads",
    async (outcome) => {
      const [first, second] = apps();
      expect(first.state.connection).not.toBe(second.state.connection);
      const pending = deferred<ReturnType<typeof wire>>();
      let firstCalls = 0;
      (call_rpc as jest.Mock).mockImplementation((connection) => {
        if (connection === first.state.connection) {
          firstCalls++;
          return pending.promise;
        }
        return Promise.resolve(spec.reply(false));
      });
      const view = render(
        <ZMKAppProvider value={first}>
          <Component />
        </ZMKAppProvider>,
      );
      await waitFor(() => expect(firstCalls).toBe(1));
      view.rerender(
        <ZMKAppProvider value={second}>
          <Component />
        </ZMKAppProvider>,
      );
      await spec.marker(false);
      await act(async () => {
        if (outcome === "success") pending.resolve(spec.reply(true));
        else pending.reject(new Error("retired load failed"));
      });
      expect(await spec.marker(false)).toBeInTheDocument();
      expect(screen.queryByText(/retired load failed/)).not.toBeInTheDocument();
    },
  );

  it("keeps the new refresh loading when an old load finishes", async () => {
    const [first, second] = apps();
    const old = deferred<ReturnType<typeof wire>>();
    const fresh = deferred<ReturnType<typeof wire>>();
    let firstCalls = 0,
      secondCalls = 0;
    (call_rpc as jest.Mock).mockImplementation((connection) => {
      if (connection === first.state.connection) {
        firstCalls++;
        return old.promise;
      }
      return ++secondCalls === 1
        ? Promise.resolve(spec.reply(false))
        : fresh.promise;
    });
    const view = render(
      <ZMKAppProvider value={first}>
        <Component />
      </ZMKAppProvider>,
    );
    await waitFor(() => expect(firstCalls).toBe(1));
    view.rerender(
      <ZMKAppProvider value={second}>
        <Component />
      </ZMKAppProvider>,
    );
    await spec.marker(false);
    fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
    await waitFor(() => expect(secondCalls).toBe(2));
    await act(async () => old.resolve(spec.reply(true)));
    expect(screen.getByRole("button", { name: /Refresh/ })).toBeDisabled();
    await act(async () => fresh.resolve(spec.reply(false)));
    expect(await spec.marker(false)).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
  });

  it.each(["success", "error"])(
    "retires an old mutation %s and its follow-up read",
    async (outcome) => {
      const [first, second] = apps();
      const pending = deferred<ReturnType<typeof wire>>();
      let firstCalls = 0;
      (call_rpc as jest.Mock).mockImplementation((connection, raw) => {
        const request = decode(raw);
        if (connection === first.state.connection) {
          firstCalls++;
          return spec.read(request)
            ? Promise.resolve(spec.reply(true))
            : pending.promise;
        }
        return Promise.resolve(spec.reply(false));
      });
      const view = render(
        <ZMKAppProvider value={first}>
          <Component />
        </ZMKAppProvider>,
      );
      await spec.marker(true);
      spec.action();
      await waitFor(() => expect(firstCalls).toBe(2));
      view.rerender(
        <ZMKAppProvider value={second}>
          <Component />
        </ZMKAppProvider>,
      );
      await spec.marker(false);
      await act(async () => {
        if (outcome === "success") pending.resolve(spec.success());
        else pending.reject(new Error("retired mutation failed"));
      });
      expect(await spec.marker(false)).toBeInTheDocument();
      expect(firstCalls).toBe(2);
      expect(
        screen.queryByText(/retired mutation failed/),
      ).not.toBeInTheDocument();
      expect(window.alert).not.toHaveBeenCalled();
    },
  );

  it("retires an unmounted load error without affecting a new panel", async () => {
    const [first, second] = apps();
    const pending = deferred<ReturnType<typeof wire>>();
    let firstCalls = 0;
    (call_rpc as jest.Mock).mockImplementation((connection) => {
      if (connection === first.state.connection) {
        firstCalls++;
        return pending.promise;
      }
      return Promise.resolve(spec.reply(false));
    });
    const view = render(
      <ZMKAppProvider value={first}>
        <Component />
      </ZMKAppProvider>,
    );
    await waitFor(() => expect(firstCalls).toBe(1));
    view.unmount();
    render(
      <ZMKAppProvider value={second}>
        <Component />
      </ZMKAppProvider>,
    );
    await spec.marker(false);
    await act(async () => pending.reject(new Error("unmounted load failed")));
    expect(await spec.marker(false)).toBeInTheDocument();
    expect(screen.queryByText(/unmounted load failed/)).not.toBeInTheDocument();
  });
});
