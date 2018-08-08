// Copyright 2017-2018 Csaba Molnar, Daniel Butum
#include "DlgHumanReadableTextCommandlet.h"

#include "Paths.h"
#include "PlatformFilemanager.h"
#include "GenericPlatformFile.h"
#include "DlgManager.h"
#include "Package.h"
#include "FileHelper.h"
#include "FileHelpers.h"

#include "Nodes/DlgNode_Speech.h"
#include "DlgJsonWriter.h"
#include "DlgNode_SpeechSequence.h"
#include "DialogueEditor/Nodes/DialogueGraphNode.h"
#include "DlgJsonParser.h"
#include "DlgCommandletHelper.h"


DEFINE_LOG_CATEGORY(LogDlgHumanReadableTextCommandlet);

const TCHAR* UDlgHumanReadableTextCommandlet::FileExtension = TEXT(".dlg_human.json");

UDlgHumanReadableTextCommandlet::UDlgHumanReadableTextCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}


int32 UDlgHumanReadableTextCommandlet::Main(const FString& Params)
{
	UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Starting"));

	// Parse command line - we're interested in the param vals
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamVals;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches, ParamVals);

	bool bExport = false;
	bool bImport = false;

	// Set the output directory
	const FString* OutputInputDirectoryVal = ParamVals.Find(FString(TEXT("OutputInputDirectory")));
	if (OutputInputDirectoryVal == nullptr)
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("Did not provide argument -OutputInputDirectory=<Path>"));
		return -1;
	}
	OutputInputDirectory = *OutputInputDirectoryVal;

	if (OutputInputDirectory.IsEmpty())
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("OutputInputDirectory is empty, please provide a non empty one with -OutputInputDirectory=<Path>"));
		return -1;
	}

	// Make it absolute
	if (FPaths::IsRelative(OutputInputDirectory))
	{
		OutputInputDirectory = FPaths::Combine(FPaths::ProjectDir(), OutputInputDirectory);
	}

	if (Switches.Contains(TEXT("Export")))
	{
		bExport = true;
	}
	else if (Switches.Contains("Import"))
	{
		bImport = true;
	}
	if (!bExport && !bImport)
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("Did not choose any operationg. Either -export OR -import"));
		return -1;
	}

	// Create destination directory
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*OutputInputDirectory) && PlatformFile.CreateDirectoryTree(*OutputInputDirectory))
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Creating OutputInputDirectory = `%s`"), *OutputInputDirectory);
	}

	UDlgManager::LoadAllDialoguesIntoMemory();

	if (bExport)
		return Export();
	else if (bImport)
		return Import();

	return 0;
}

int32 UDlgHumanReadableTextCommandlet::Export()
{
	UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Exporting to = `%s`"), *OutputInputDirectory);

	// Some Dialogues may be unclean?
	FDlgCommandletHelper::SaveAllDialogues();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const TArray<UDlgDialogue*> AllDialogues = UDlgManager::GetAllDialoguesFromMemory();
	for (const UDlgDialogue* Dialogue : AllDialogues)
	{
		UPackage* Package = Dialogue->GetOutermost();
		check(Package);
		const FString OriginalDialoguePath = Package->GetPathName();
		FString DialoguePath = OriginalDialoguePath;

		// Only export game dialogues
		if (!FDlgCommandletHelper::IsDialoguePathInProjectDirectory(DialoguePath))
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("Dialogue = `%s` is not in the game directory, ignoring"), *DialoguePath);
			continue;
		}

		verify(DialoguePath.RemoveFromStart(TEXT("/Game")));
		const FString FileName = FPaths::GetBaseFilename(DialoguePath);
		const FString Directory = FPaths::GetPath(DialoguePath);

		// Ensure directory tree
		const FString FileSystemDirectoryPath = OutputInputDirectory / Directory;
		if (!PlatformFile.DirectoryExists(*FileSystemDirectoryPath) && PlatformFile.CreateDirectoryTree(*FileSystemDirectoryPath))
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Creating directory = `%s`"), *FileSystemDirectoryPath);
		}

		// Export file
		FDlgJsonWriter JsonWriter;
		FDlgDialogue_FormatHumanReadable ExportFormat;
		if (!ExportDialogueToHumanReadableFormat(*Dialogue, ExportFormat))
		{
			continue;
		}
		JsonWriter.Write(FDlgDialogue_FormatHumanReadable::StaticStruct(), &ExportFormat);

		const FString FileSystemFilePath = FileSystemDirectoryPath / FileName + FileExtension;
		if (JsonWriter.ExportToFile(FileSystemFilePath))
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Writing file = `%s` for Dialogue = `%s` "), *FileSystemFilePath, *OriginalDialoguePath);
		}
		else
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("FAILED to write file = `%s` for Dialogue = `%s`"), *FileSystemFilePath, *OriginalDialoguePath);
		}
	}

	return 0;
}

int32 UDlgHumanReadableTextCommandlet::Import()
{
	UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Importing from = `%s`"), *OutputInputDirectory);

	PackagesToSave.Empty();
	TMap<FGuid, UDlgDialogue*> DialoguesMap = UDlgManager::GetAllDialoguesGuidMap();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Find all files
	TArray<FString> FoundFiles;
	PlatformFile.FindFilesRecursively(FoundFiles, *OutputInputDirectory, FileExtension);
	if (FoundFiles.Num() == 0)
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("FAILED import, could not find any files with the extension = `%s` inside the directory = `%s`"), FileExtension, *OutputInputDirectory);
		return -1;
	}

	for (const FString& File : FoundFiles)
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Display, TEXT("Reading file = `%s` "), *File);

		FDlgJsonParser JsonParser;
		JsonParser.InitializeParser(File);
		if (!JsonParser.IsValidFile())
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("FAILED to read file = `%s`"), *File);
			continue;
		}

		FDlgDialogue_FormatHumanReadable HumanFormat;
		JsonParser.ReadAllProperty(FDlgDialogue_FormatHumanReadable::StaticStruct(), &HumanFormat);
		if (!JsonParser.IsValidFile())
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Error, TEXT("File = `%s` is not a valid JSON file"), *File);
			continue;
		}


		// Find Dialogue
		UDlgDialogue** DialoguePtr = DialoguesMap.Find(HumanFormat.DialogueGuid);
		if (DialoguePtr == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet,
				Error,
				TEXT("Can't find Dialogue for GUID = `%s`, DialogueName = `%s` from File = `%s`"),
				*HumanFormat.DialogueGuid.ToString(), *HumanFormat.DialogueName.ToString(), *File);
			continue;
		}

		// Import
		UDlgDialogue* Dialogue = *DialoguePtr;
		if (ImportHumanReadableFormatIntoDialogue(HumanFormat, Dialogue))
		{
			PackagesToSave.Add(Dialogue->GetOutermost());
		}
	}

	return UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false) == true ? 0 : -1;
}

bool UDlgHumanReadableTextCommandlet::ExportDialogueToHumanReadableFormat(const UDlgDialogue& Dialogue, FDlgDialogue_FormatHumanReadable& OutFormat)
{
	OutFormat.DialogueName = Dialogue.GetDlgFName();
	OutFormat.DialogueGuid = Dialogue.GetDlgGuid();

	const TArray<UDlgNode*>& Nodes = Dialogue.GetNodes();
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); NodeIndex++)
	{
		const UDlgNode* Node = Nodes[NodeIndex];
		if (const UDlgNode_Speech* NodeSpeech = Cast<UDlgNode_Speech>(Node))
		{
			// Speech
			if (NodeSpeech->IsVirtualParent())
			{
				continue;
			}

			// Fill Nodes
			FDlgNodeSpeech_FormatHumanReadable ExportNode;
			ExportNode.NodeIndex = NodeIndex;
			ExportNode.Text = NodeSpeech->GetNodeUnformattedText();

			// Fill Edges
			for (const FDlgEdge& Edge : Node->GetNodeChildren())
			{
				if (!Edge.IsValid())
				{
					continue;
				}

				FDlgEdge_FormatHumanReadable ExportEdge;
				ExportEdge.TargetNodeIndex = Edge.TargetIndex;
				ExportEdge.Text = Edge.Text;
				ExportNode.Edges.Add(ExportEdge);
			}

			OutFormat.SpeechNodes.Add(ExportNode);
		}
		else if (const UDlgNode_SpeechSequence* NodeSpeechSequence = Cast<UDlgNode_SpeechSequence>(Node))
		{
			// Speech Sequence

			FDlgNodeSpeechSequence_FormatHumanReadable ExportNode;
			ExportNode.NodeIndex = NodeIndex;

			// Fill sequence
			for (const FDlgSpeechSequenceEntry& Entry : NodeSpeechSequence->GetNodeSpeechSequence())
			{
				FDlgSpeechSequenceEntry_FormatHumanReadable ExportEntry;
				ExportEntry.EdgeText = Entry.EdgeText;
				ExportEntry.Text = Entry.Text;
				ExportNode.Sequence.Add(ExportEntry);
			}

			// Fill Edges
			for (const FDlgEdge& Edge : Node->GetNodeChildren())
			{
				if (!Edge.IsValid())
				{
					continue;
				}

				FDlgEdge_FormatHumanReadable ExportEdge;
				ExportEdge.TargetNodeIndex = Edge.TargetIndex;
				ExportEdge.Text = Edge.Text;
				ExportNode.Edges.Add(ExportEdge);
			}

			OutFormat.SpeechSequenceNodes.Add(ExportNode);
		}
		else
		{
			// not supported
		}

		// Sanity check
		if (const UDialogueGraphNode_Base* GraphNode = Cast<UDialogueGraphNode_Base>(Node->GetGraphNode()))
		{
			GraphNode->CheckAll();
		}
	}

	return true;
}

bool UDlgHumanReadableTextCommandlet::ExportNodeToContext(const UDlgNode* Node, FDlgNodeContext_FormatHumanReadable& OutContext)
{
	if (Node == nullptr)
	{
		return false;
	}

	const UEdGraphNode* GraphNode = Node->GetGraphNode();
	if (GraphNode == nullptr)
	{
		return false;
	}

	const UDialogueGraphNode* DialogueGraphNode = Cast<UDialogueGraphNode>(GraphNode);
	if (DialogueGraphNode == nullptr)
	{
		return false;
	}

	for (const UDialogueGraphNode* ParentNode : DialogueGraphNode->GetParentNodes())
	{
		OutContext.ParentNodeIndices.Add(ParentNode->GetDialogueNodeIndex());
	}
	for (const UDialogueGraphNode* ChildNode : DialogueGraphNode->GetChildNodes())
	{
		OutContext.ChildNodeIndices.Add(ChildNode->GetDialogueNodeIndex());
	}
	OutContext.Speaker = Node->GetNodeParticipantName();

	return true;
}

bool UDlgHumanReadableTextCommandlet::ImportHumanReadableFormatIntoDialogue(const FDlgDialogue_FormatHumanReadable& Format, UDlgDialogue* Dialogue)
{
	verify(Dialogue);

	bool bModified = false;
	if (Format.SpeechNodes.Num() == 0 && Format.SpeechSequenceNodes.Num() == 0)
	{
		UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("ImportHumanReadableFormatIntoDialogue: No data to import for Dialogue = `%s`"), *Dialogue->GetPathName());
		return false;
	}

	// Speech nodes
	for (const FDlgNodeSpeech_FormatHumanReadable& HumanNode : Format.SpeechNodes)
	{
		// Node
		UDlgNode* Node = Dialogue->GetMutableNode(HumanNode.NodeIndex);
		if (Node == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("Invalid node index = %d, in Dialogue = `%s`. Ignoring."), HumanNode.NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		UDlgNode_Speech* NodeSpeech = Cast<UDlgNode_Speech>(Node);
		if (NodeSpeech == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("Node index = %d  is not a UDlgNode_Speech, in Dialogue = `%s`. Ignoring."), HumanNode.NodeIndex, *Dialogue->GetPathName());
			continue;
		}
		if (NodeSpeech->IsVirtualParent())
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("Node index = %d  is not VirtualParent, in Dialogue = `%s`. Ignoring."), HumanNode.NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		// Node Text changed
		if (!NodeSpeech->GetNodeUnformattedText().EqualTo(HumanNode.Text))
		{
			NodeSpeech->SetNodeUnformattedText(HumanNode.Text);
			bModified = true;
		}

		UDialogueGraphNode* GraphNode = Cast<UDialogueGraphNode>(Node->GetGraphNode());
		if (GraphNode == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("Invalid UDialogueGraphNode for Node index = %d in Dialogue = `%s`. Ignoring."), HumanNode.NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		// Edges
		if (SetGraphNodesNewEdgesText(GraphNode, HumanNode.Edges, HumanNode.NodeIndex, Dialogue))
		{
			bModified = true;
		}
		GraphNode->CheckAll();
	}

	// Speech sequence nodes
	for (const FDlgNodeSpeechSequence_FormatHumanReadable& HumanSpeechSequence : Format.SpeechSequenceNodes)
	{
		// Node
		UDlgNode* Node = Dialogue->GetMutableNode(HumanSpeechSequence.NodeIndex);
		if (Node == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet,
				Warning,
				TEXT("Invalid node index = %d, in Dialogue = `%s`. Ignoring."),
				HumanSpeechSequence.NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		UDlgNode_SpeechSequence* NodeSpeechSequence = Cast<UDlgNode_SpeechSequence>(Node);
		if (NodeSpeechSequence == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet,
				Warning,
				TEXT("Node index = %d  is not a UDlgNode_SpeechSequence, in Dialogue = `%s`. Ignoring."),
				HumanSpeechSequence.NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		// Sequence nodes
		TArray<FDlgSpeechSequenceEntry>& SequenceArray = *NodeSpeechSequence->GetMutableNodeSpeechSequence();
		for (int32 SequenceIndex = 0; SequenceIndex < SequenceArray.Num() && SequenceIndex < HumanSpeechSequence.Sequence.Num(); SequenceIndex++)
		{
			const FDlgSpeechSequenceEntry_FormatHumanReadable& HumanSequence = HumanSpeechSequence.Sequence[SequenceIndex];

			// Edge changed
			if (!SequenceArray[SequenceIndex].EdgeText.EqualTo(HumanSequence.EdgeText))
			{
				SequenceArray[SequenceIndex].EdgeText = HumanSequence.EdgeText;
				bModified = true;
			}

			// Text changed
			if (!SequenceArray[SequenceIndex].Text.EqualTo(HumanSequence.Text))
			{
				SequenceArray[SequenceIndex].Text = HumanSequence.Text;
				bModified = true;
			}
		}

		UDialogueGraphNode* GraphNode = Cast<UDialogueGraphNode>(Node->GetGraphNode());
		if (GraphNode == nullptr)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet, Warning, TEXT("Invalid UDialogueGraphNode for Node index = %d in Dialogue = `%s`. Ignoring."), HumanSpeechSequence.NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		// Edges from big node
		if (SetGraphNodesNewEdgesText(GraphNode, HumanSpeechSequence.Edges, HumanSpeechSequence.NodeIndex, Dialogue))
		{
			bModified = true;
		}
		GraphNode->CheckAll();
	}

	if (bModified)
	{
		Dialogue->Modify();
		Dialogue->MarkPackageDirty();
	}

	return bModified;
}

bool UDlgHumanReadableTextCommandlet::SetGraphNodesNewEdgesText(UDialogueGraphNode* GraphNode, const TArray<FDlgEdge_FormatHumanReadable>& Edges, const int32 NodeIndex, const UDlgDialogue* Dialogue)
{
	bool bModified = false;

	for (const FDlgEdge_FormatHumanReadable& HumanEdge : Edges)
	{
		const int32 EdgeIndex = GraphNode->GetChildEdgeIndexForChildNodeIndex(HumanEdge.TargetNodeIndex);
		if (EdgeIndex < 0)
		{
			UE_LOG(LogDlgHumanReadableTextCommandlet,
				Warning,
				TEXT("Invalid EdgeIndex = %d for Node index = %d in Dialogue = `%s`. Ignoring."),
				HumanEdge.TargetNodeIndex, NodeIndex, *Dialogue->GetPathName());
			continue;
		}

		// Edge Changed
		if (!GraphNode->GetDialogueNode().GetNodeChildren()[EdgeIndex].Text.EqualTo(HumanEdge.Text))
		{
			GraphNode->SetEdgeTextAt(EdgeIndex, HumanEdge.Text);
			bModified = true;
		}
	}

	return bModified;
}

bool UDlgHumanReadableTextCommandlet::IsEdgeTextDefault(const FText& EdgeText)
{
	return UDlgDialogue::EdgeTextFinish.EqualToCaseIgnored(EdgeText) || UDlgDialogue::EdgeTextNext.EqualToCaseIgnored(EdgeText);
}