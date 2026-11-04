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
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";
import { ProfileManager } from "../../web/src/components/ProfileManager";
import { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import {
  Request,
  Response,
} from "../../web/src/proto/zmk/ble_management/ble_management";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));
const peer = { addressType: 1, address: new Uint8Array([6, 5, 4, 3, 2, 1]) };
const wire = (value: Parameters<typeof Response.create>[0]) => ({
  custom: {
    call: { payload: Response.encode(Response.create(value)).finish() },
  },
});
const profiles = (identity = peer) =>
  wire({
    getProfiles: {
      maxProfiles: 5,
      profiles: [
        {
          index: 1,
          name: "fixture-host",
          address: "01:02:03:04:05:06",
          isOpen: false,
          isActive: true,
          peerIdentity: identity,
        },
      ],
    },
  });
function mountProfiles() {
  const app = createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] });
  type Subscription = Parameters<typeof app.onNotification>[0];
  const subscriptions = new Set<Subscription>();
  jest.mocked(app.onNotification).mockImplementation((subscription) => {
    subscriptions.add(subscription);
    return () => {
      subscriptions.delete(subscription);
    };
  });
  render(
    <ZMKAppProvider value={app}>
      <ProfileManager />
    </ZMKAppProvider>,
  );
  return (state: LockState) =>
    act(() => {
      for (const subscription of [...subscriptions]) {
        if (subscription.type === "core")
          subscription.callback({ lockStateChanged: state });
      }
    });
}
beforeEach(() => {
  (call_rpc as jest.Mock).mockReset();
  jest.spyOn(window, "confirm").mockReturnValue(true);
  jest.spyOn(console, "error").mockImplementation(() => {});
});
afterEach(() => jest.restoreAllMocks());

it("sends the complete typed peer identity when unpairing", async () => {
  let request: Request | undefined;
  (call_rpc as jest.Mock).mockImplementation(async (_connection, raw) => {
    const decoded = Request.decode(raw.custom.call.payload);
    if (decoded.getProfiles) return profiles();
    request = decoded;
    return wire({ error: { message: "Fixture refusal" } });
  });
  mountProfiles();
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByRole("button", { name: /Unpair/ }));
  await screen.findByText(/Fixture refusal/);
  expect(request?.unpairProfile).toEqual({ index: 1, expectedPeer: peer });
});
it("retains the original edit identity across a refreshed replacement", async () => {
  let reads = 0;
  let request: Request | undefined;
  (call_rpc as jest.Mock).mockImplementation(async (_connection, raw) => {
    const decoded = Request.decode(raw.custom.call.payload);
    if (decoded.getProfiles)
      return profiles(++reads === 1 ? peer : { ...peer, addressType: 0 });
    request = decoded;
    return wire({ error: { message: "Peer replaced" } });
  });
  mountProfiles();
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.change(screen.getByRole("textbox"), {
    target: { value: "new-name" },
  });
  fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
  await waitFor(() => expect(reads).toBe(2));
  await waitFor(() =>
    expect(screen.getByRole("button", { name: /Save/ })).toBeEnabled(),
  );
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await screen.findByText(/Peer replaced/);
  expect(request?.setProfileName).toEqual({
    index: 1,
    name: "new-name",
    expectedPeer: peer,
  });
});
it("redacts peer data on lock and retires a pending profile response", async () => {
  let reads = 0;
  let finish!: (reply: ReturnType<typeof wire>) => void;
  (call_rpc as jest.Mock).mockImplementation((_connection, raw) => {
    if (!Request.decode(raw.custom.call.payload).getProfiles)
      throw new Error("Unexpected request");
    if (++reads === 2)
      return new Promise((resolve) => {
        finish = resolve;
      });
    return Promise.resolve(profiles());
  });
  const notify = mountProfiles();
  await screen.findByText("01:02:03:04:05:06");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
  await waitFor(() => expect(finish).toBeDefined());
  notify(LockState.ZMK_STUDIO_CORE_LOCK_STATE_LOCKED);
  expect(screen.queryByRole("textbox")).not.toBeInTheDocument();
  expect(screen.queryByText("01:02:03:04:05:06")).not.toBeInTheDocument();
  await act(async () => finish(profiles()));
  expect(screen.queryByText("01:02:03:04:05:06")).not.toBeInTheDocument();
  notify(LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED);
  await screen.findByText("01:02:03:04:05:06");
  expect(reads).toBe(3);
});
it("rejects over 31 UTF-8 bytes and accepts the exact name boundary", async () => {
  const requests: Request[] = [];
  (call_rpc as jest.Mock).mockImplementation(async (_connection, raw) => {
    const request = Request.decode(raw.custom.call.payload);
    if (request.getProfiles) return profiles();
    requests.push(request);
    return wire({ setProfileName: { success: true } });
  });
  mountProfiles();
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.change(screen.getByRole("textbox"), {
    target: { value: "é".repeat(16) },
  });
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await screen.findByText(/31 UTF-8 bytes/);
  expect(requests).toHaveLength(0);
  fireEvent.change(screen.getByRole("textbox"), {
    target: { value: "é".repeat(15) + "a" },
  });
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await waitFor(() => expect(requests).toHaveLength(1));
  expect(requests[0].setProfileName?.name).toBe("é".repeat(15) + "a");
});
it("refuses mutations if an older firmware response has no peer identity", async () => {
  let calls = 0;
  (call_rpc as jest.Mock).mockImplementation(async () => {
    calls++;
    return wire({
      getProfiles: {
        maxProfiles: 5,
        profiles: [
          { index: 1, name: "legacy-host", isOpen: false, isActive: true },
        ],
      },
    });
  });
  mountProfiles();
  await screen.findByText("legacy-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  await screen.findByText(/Unlock Studio and refresh/);
  expect(screen.queryByRole("textbox")).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole("button", { name: /Unpair/ }));
  expect(calls).toBe(1);
});
