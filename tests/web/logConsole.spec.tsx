import { render, screen } from "@testing-library/react";
import { ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { ProfileManager } from "../../web/src/components/ProfileManager";
import { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";

afterEach(() => jest.restoreAllMocks());

it("keeps profile failure details in the UI and excludes them from the console", async () => {
  const errors = jest.spyOn(console, "error").mockImplementation(() => {});
  jest
    .spyOn(ZMKCustomSubsystem.prototype, "callRPC")
    .mockRejectedValue(new TypeError("private-profile-name"));
  const app = createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] });
  render(
    <ZMKAppProvider value={app}>
      <ProfileManager />
    </ZMKAppProvider>,
  );
  await screen.findByText(/private-profile-name/);
  expect(errors).toHaveBeenCalledWith("Failed to load profiles:", "TypeError");
  expect(JSON.stringify(errors.mock.calls)).not.toContain(
    "private-profile-name",
  );
});
