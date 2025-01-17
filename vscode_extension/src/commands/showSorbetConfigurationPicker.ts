import { QuickPickItem, window } from "vscode";
import { SorbetLspConfig } from "../config";
import { SorbetExtensionContext } from "../sorbetExtensionContext";

export interface LspConfigQuickPickItem extends QuickPickItem {
  lspConfig?: SorbetLspConfig;
}

/**
 * Show Sorbet Configuration picker.
 */
export async function showSorbetConfigurationPicker(
  context: SorbetExtensionContext,
): Promise<void> {
  const {
    configuration: { activeLspConfig, lspConfigs },
  } = context;

  const items: LspConfigQuickPickItem[] = lspConfigs.map((lspConfig) => ({
    label: `${lspConfig.isEqualTo(activeLspConfig) ? "• " : ""}${
      lspConfig.name
    }`,
    description: lspConfig.description,
    detail: lspConfig.command.join(" "),
    lspConfig,
  }));
  items.push({
    label: `${activeLspConfig ? "" : "• "}Disable Sorbet`,
    description: "Disable the Sorbet extension",
  });

  const selectedItem = await window.showQuickPick(items, {
    placeHolder: "Select a Sorbet configuration",
  });
  if (selectedItem) {
    const { lspConfig } = selectedItem;
    if (lspConfig) {
      context.configuration.setActiveLspConfigId(lspConfig.id);
    } else {
      context.configuration.setEnabled(false);
    }
  }
}
