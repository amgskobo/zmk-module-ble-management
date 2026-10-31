import { act, fireEvent, render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import { SplitManager } from "../../web/src/components/SplitManager";
import { Response } from "../../web/src/proto/zmk/ble_management/ble_management";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));

const wire = (response: Parameters<typeof Response.create>[0]) => ({
  custom: {
    call: { payload: Response.encode(Response.create(response)).finish() },
  },
});
const splitInfo = () => wire({ getSplitInfo: { info: { isSplit: true } } });
const app = () =>
  createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] });

beforeEach(() => {
  (call_rpc as jest.Mock).mockReset();
  jest.spyOn(console, "error").mockImplementation(() => {});
});
afterEach(() => jest.restoreAllMocks());

it.each([
  ["absent custom payload", {}],
  ["empty response payload", wire({})],
  ["unrelated response kind", wire({ getProfiles: {} })],
  ["split response without info", wire({ getSplitInfo: {} })],
])("exposes and recovers from an initial %s", async (_name, response) => {
  const current = app();
  (call_rpc as jest.Mock)
    .mockResolvedValueOnce(response)
    .mockResolvedValueOnce(splitInfo());
  const view = render(
    <ZMKAppProvider value={current}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  expect(view.container.firstChild).toBeNull();
  await screen.findByText(/Failed to load split info/);
  const refresh = screen.getByRole("button", { name: /Refresh/ });
  expect(refresh).toBeEnabled();
  fireEvent.click(refresh);
  await screen.findByText("Peripheral");
  expect(screen.queryByText(/Failed to load split info/)).toBeNull();
  expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
  expect(call_rpc).toHaveBeenCalledTimes(2);
  expect((call_rpc as jest.Mock).mock.calls[1][0]).toBe(
    current.state.connection,
  );
});

it("ignores a retired absent-payload response while the new connection is pending", async () => {
  const first = app();
  const second = app();
  let resolveOld!: (value: object) => void;
  let resolveNew!: (value: object) => void;
  (call_rpc as jest.Mock)
    .mockReturnValueOnce(new Promise((done) => (resolveOld = done)))
    .mockReturnValueOnce(new Promise((done) => (resolveNew = done)));
  const view = render(
    <ZMKAppProvider value={first}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  view.rerender(
    <ZMKAppProvider value={second}>
      <SplitManager />
    </ZMKAppProvider>,
  );
  await act(async () => resolveOld({}));
  expect(view.container.firstChild).toBeNull();
  await act(async () => resolveNew(splitInfo()));
  expect(screen.getByText("Peripheral")).toBeInTheDocument();
  expect(screen.queryByText(/Failed to load split info/)).toBeNull();
  expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
});
