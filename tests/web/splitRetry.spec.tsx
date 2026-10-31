import { act, fireEvent, render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import { SplitManager } from "../../web/src/components/SplitManager";
import {
  Request,
  Response,
} from "../../web/src/proto/zmk/ble_management/ble_management";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));

const wire = (response: Parameters<typeof Response.create>[0]) => ({
  custom: {
    call: { payload: Response.encode(Response.create(response)).finish() },
  },
});
const info = (isSplit: boolean, isCentral = true) =>
  wire({
    getSplitInfo: {
      info: { isSplit, isCentral, centralBonded: true },
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

beforeEach(() => {
  (call_rpc as jest.Mock).mockReset();
  jest.spyOn(console, "error").mockImplementation(() => {});
});
afterEach(() => {
  jest.restoreAllMocks();
  jest.useRealTimers();
});

it("shows an initial rejection and retries successfully on the same connection", async () => {
  const current = app();
  const retry = deferred<ReturnType<typeof wire>>();
  (call_rpc as jest.Mock)
    .mockRejectedValueOnce(new Error("initial split read failed"))
    .mockReturnValueOnce(retry.promise);
  const view = render(
    <ZMKAppProvider value={current}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  expect(view.container.firstChild).toBeNull();
  await screen.findByText(/initial split read failed/);
  const refresh = screen.getByRole("button", { name: /Refresh/ });
  expect(refresh).toBeEnabled();
  fireEvent.click(refresh);
  expect(refresh).toBeDisabled();
  expect(screen.getByText(/Loading split information/)).toBeInTheDocument();
  expect(
    screen.queryByText(/initial split read failed/),
  ).not.toBeInTheDocument();
  await act(async () => retry.resolve(info(true)));
  expect(screen.getByText("Central")).toBeInTheDocument();
  expect(refresh).toBeEnabled();
  expect(call_rpc).toHaveBeenCalledTimes(2);
  for (const [connection, raw] of (call_rpc as jest.Mock).mock.calls) {
    expect(connection).toBe(current.state.connection);
    expect(Request.decode(raw.custom.call.payload).getSplitInfo).toBeDefined();
  }
});

it("exposes the actual SDK initial timeout and recovers on the same connection", async () => {
  jest.useFakeTimers();
  const current = app();
  (call_rpc as jest.Mock)
    .mockReturnValueOnce(new Promise(() => {}))
    .mockResolvedValueOnce(info(true, false));
  const view = render(
    <ZMKAppProvider value={current}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  expect(view.container.firstChild).toBeNull();
  await act(async () => {
    await jest.advanceTimersByTimeAsync(5000);
  });
  expect(screen.getByText(/Operation timed out/)).toBeInTheDocument();
  fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
  await act(async () => {});
  expect(screen.getByText("Peripheral")).toBeInTheDocument();
  expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
  expect(call_rpc).toHaveBeenCalledTimes(2);
  expect((call_rpc as jest.Mock).mock.calls[1][0]).toBe(
    current.state.connection,
  );
  view.unmount();
});

it("keeps retry errors visible and allows a later successful recovery", async () => {
  (call_rpc as jest.Mock)
    .mockResolvedValueOnce(
      wire({ error: { message: "split read unavailable" } }),
    )
    .mockRejectedValueOnce(new Error("retry split read failed"))
    .mockResolvedValueOnce(info(true));
  render(
    <ZMKAppProvider value={app()}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  await screen.findByText(/split read unavailable/);
  fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
  await screen.findByText(/retry split read failed/);
  expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
  fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
  await screen.findByText("Central");
  expect(screen.queryByText(/retry split read failed/)).not.toBeInTheDocument();
  expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
  expect(call_rpc).toHaveBeenCalledTimes(3);
});

it("resets recovery state and ignores a retired retry error after replacement", async () => {
  const first = app();
  const second = app();
  const oldRetry = deferred<ReturnType<typeof wire>>();
  const fresh = deferred<ReturnType<typeof wire>>();
  let firstCalls = 0;
  (call_rpc as jest.Mock).mockImplementation((connection) => {
    if (connection === first.state.connection) {
      return ++firstCalls === 1
        ? Promise.reject(new Error("first initial failure"))
        : oldRetry.promise;
    }
    return fresh.promise;
  });
  const view = render(
    <ZMKAppProvider value={first}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  await screen.findByText(/first initial failure/);
  fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
  expect(firstCalls).toBe(2);
  view.rerender(
    <ZMKAppProvider value={second}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  expect(view.container.firstChild).toBeNull();
  await act(async () => oldRetry.reject(new Error("retired retry failed")));
  expect(view.container.firstChild).toBeNull();
  await act(async () => fresh.resolve(info(true, false)));
  expect(screen.getByText("Peripheral")).toBeInTheDocument();
  expect(screen.queryByText(/retired retry failed/)).not.toBeInTheDocument();
  expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
});

it.each(["initial", "recovery"])(
  "hides a successful non-split result from the %s read",
  async (phase) => {
    const response = deferred<ReturnType<typeof wire>>();
    if (phase === "recovery") {
      (call_rpc as jest.Mock).mockRejectedValueOnce(new Error("try again"));
    }
    (call_rpc as jest.Mock).mockReturnValueOnce(response.promise);
    const view = render(
      <ZMKAppProvider value={app()}>
        <SplitManager />
      </ZMKAppProvider>,
    );
    if (phase === "recovery") {
      await screen.findByText(/try again/);
      fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
    } else {
      expect(view.container.firstChild).toBeNull();
    }
    await act(async () => response.resolve(info(false)));
    expect(view.container.firstChild).toBeNull();
    expect(screen.queryByRole("button", { name: /Refresh/ })).toBeNull();
  },
);
