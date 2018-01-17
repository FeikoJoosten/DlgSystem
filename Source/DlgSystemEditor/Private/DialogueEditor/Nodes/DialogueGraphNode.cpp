// Copyright 2017-2018 Csaba Molnar, Daniel Butum
#include "DialogueGraphNode.h"

#include "Editor/EditorEngine.h"
#include "GenericCommands.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Font.h"
#include "MultiBoxBuilder.h"

#include "DlgSystemEditorPrivatePCH.h"
#include "DlgDialogue.h"
#include "DialogueEditor/DialogueEditorCommands.h"
#include "DlgEditorSettings.h"

#define LOCTEXT_NAMESPACE "DialogueGraphNode"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin UObject interface
void UDialogueGraphNode::PostLoad()
{
	Super::PostLoad();

	// Fixup any DialogueNode back pointers that may be out of date
	if (DialogueNode)
	{
		DialogueNode->SetGraphNode(this);
		DialogueNode->SetFlags(RF_Transactional);
	}
}

void UDialogueGraphNode::PostEditImport()
{
	RegisterListeners();

	// Make sure this DialogueNode is owned by the Dialogue it's being pasted into.
	// The paste can come from another Dialogue
	ResetDialogueNodeOwner();
}

void UDialogueGraphNode::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.Property == nullptr)
	{
		return;
	}

	CheckAll();
	ApplyCompilerWarnings();
}

void UDialogueGraphNode::PostEditChangeChainProperty(struct FPropertyChangedChainEvent& PropertyChangedEvent)
{
	Super::PostEditChangeChainProperty(PropertyChangedEvent);
}

bool UDialogueGraphNode::Modify(bool bAlwaysMarkDirty)
{
	if (!CanModify())
	{
		return false;
	}

	bool bWasModified = Super::Modify(bAlwaysMarkDirty);

	// Notify the Dialogue structure and the edge nodes of modification
	if (DialogueNode)
	{
		bWasModified = bWasModified && DialogueNode->Modify(bAlwaysMarkDirty);
	}

	// Special case when this method is called when the engine is starting
	if (HasOutputPin())
	{
		// Can happen when copy pasting nodes
		constexpr bool bCheckParent = false;
		for (UDialogueGraphNode_Edge* EdgeNode : GetChildEdgeNodes(bCheckParent))
		{
			bWasModified = bWasModified && EdgeNode->SuperModify();
		}
	}

	return bWasModified;
}
// End UObject interface
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin UEdGraphNode interface
FText UDialogueGraphNode::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (NodeIndex == INDEX_NONE)
	{
		return Super::GetNodeTitle(TitleType);
	}

	const FString FullString = DialogueNode->GetNodeParticipantName().ToString();
	// Display the full title
	return FText::FromString(FullString);
}

void UDialogueGraphNode::PrepareForCopying()
{
	Super::PrepareForCopying();

	// Temporarily take ownership of the DialogueNode, so that it is not deleted when cutting
	if (DialogueNode)
	{
		DialogueNode->Rename(nullptr, this, REN_DontCreateRedirectors);
	}
}

void UDialogueGraphNode::PostCopyNode()
{
	Super::PostCopyNode();
	// Make sure the DialogueNode goes back to being owned by the Dialogue after copying.
	ResetDialogueNodeOwner();
}

FString UDialogueGraphNode::GetDocumentationExcerptName() const
{
	// Default the node to searching for an excerpt named for the C++ node class name, including the U prefix.
	// This is done so that the excerpt name in the doc file can be found by find-in-files when searching for the full class name.
//	UClass* MyClass = DialogueNode != nullptr ? DialogueNode->GetClass() : this->GetClass();
//	return FString::Printf(TEXT("%s%s"), MyClass->GetPrefixCPP(), *MyClass->GetName());
	return "";
}

FText UDialogueGraphNode::GetTooltipText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("ParticipantName"), DialogueNode ? FText::FromName(DialogueNode->GetNodeParticipantName()) : FText::GetEmpty());
	Args.Add(TEXT("NodeType"), DialogueNode ? FText::FromString(DialogueNode->GetNodeTypeString()) : FText::GetEmpty());
	Args.Add(TEXT("Description"), DialogueNode ? FText::FromString(DialogueNode->GetDesc()) : FText::GetEmpty());
	return FText::Format(LOCTEXT("GraphNodeTooltip", "ParticipantName = {ParticipantName}\nType = {NodeType}\n{Description}"), Args);
}

void UDialogueGraphNode::PinConnectionListChanged(UEdGraphPin* Pin)
{
	// NOTE: GraphNode.OutputPin.LinkedTo are kept in sync with the DialogueNode.Children
	check(Pin->GetOwningNode() == this);
	check(DialogueNode->GetNodeOpenChildren_DEPRECATED().Num() == 0);

	// Input pins are ignored, as they are not reliable source of information, each node should only work with its output pins
	if (Pin->Direction == EGPD_Input)
	{
		return;
	}

	// Handle Output Pin (this node)
	check(Pin->Direction == EGPD_Output);
	const UEdGraphPin* OutputPin = GetOutputPin();
	// Only one Pin, and that pin should be our output pinOutputPin
	check(Pin == OutputPin);

	const int32 DialogueNodeChildrenNum = DialogueNode->GetNodeChildren().Num();
	const int32 GraphNodeChildrenNum = OutputPin->LinkedTo.Num();

	// Nothing added/removed, maybe something replaced?
	if (DialogueNodeChildrenNum == GraphNodeChildrenNum)
	{
#if DO_CHECK
		const FDiffNodeEdgeLinkedToPinResult& DiffResult = FindDifferenceBetweenNodeEdgesAndLinkedToPins();
		check(DiffResult.Index == INDEX_NONE);
		check(DiffResult.Type == FDiffNodeEdgeLinkedToPinResult::EDiffType::NO_DIFFERENCE);
#endif
	}
	// Some link was added/removed
	else if (DialogueNodeChildrenNum < GraphNodeChildrenNum)
	{
		// Output link added, extend the number of children
		// Not handled here, as everytime we just add at the end of the array.
		// See UDialogueGraphNode_Edge::CreateConnections
	}
	else
	{
		// One Output link removed, reduce the number of linked children
		check(DialogueNodeChildrenNum > GraphNodeChildrenNum);
		if (GraphNodeChildrenNum == 0)
		{
			// All nodes were removed, node is most likely going to be removed
			DialogueNode->RemoveAllChildren();
		}
		else
		{
			// Only one
			const FDiffNodeEdgeLinkedToPinResult DiffResult = FindDifferenceBetweenNodeEdgesAndLinkedToPins();
			check(DiffResult.Type == FDiffNodeEdgeLinkedToPinResult::EDiffType::LENGTH_MISMATCH_ONE_MORE_EDGE ||
				DiffResult.Type == FDiffNodeEdgeLinkedToPinResult::EDiffType::EDGE_NOT_MATCHING_INDEX);
			DialogueNode->RemoveChildAt(DiffResult.Index);
		}
	}
}

void UDialogueGraphNode::GetContextMenuActions(const FGraphNodeContextMenuBuilder& Context) const
{
	// These actions (commands) are handled and registered in the FDialogueEditor class
	if (Context.Node && !IsRootNode())
	{
		// Menu for right clicking on node
		Context.MenuBuilder->BeginSection("DialogoueGraphNode_BaseNodeEditCRUD");
		{
			if (IsSpeechSequenceNode())
				Context.MenuBuilder->AddMenuEntry(FDialogueEditorCommands::Get().ConvertSpeechSequenceNodeToSpeechNodes);

			Context.MenuBuilder->AddMenuEntry(FGenericCommands::Get().Delete);
//			Context.MenuBuilder->AddMenuEntry(FGenericCommands::Get().Cut);
			Context.MenuBuilder->AddMenuEntry(FGenericCommands::Get().Copy);
//			Context.MenuBuilder->AddMenuEntry(FGenericCommands::Get().Duplicate);
		}
		Context.MenuBuilder->EndSection();
	}
}

void UDialogueGraphNode::AutowireNewNode(UEdGraphPin* FromPin)
{
	// No context given, simply return
	if (FromPin == nullptr)
	{
		return;
	}

	// FromPin should not belong to this node but to the node that spawned this node.
	check(FromPin->GetOwningNode() != this);
	check(FromPin->Direction == EGPD_Output);

	UEdGraphPin* InputpIn = GetInputPin();
	const UDialogueGraphSchema* Schema = GetDialogueGraphSchema();

	// auto-connect from dragged pin to first compatible pin on the new node
	verify(Schema->TryCreateConnection(FromPin, InputpIn));
	FromPin->GetOwningNode()->NodeConnectionListChanged();
}
// End UEdGraphNode interface
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin UDialogueGraphNode_Base interface
FLinearColor UDialogueGraphNode::GetNodeBackgroundColor() const
{
	if (NodeIndex == INDEX_NONE)
	{
		return FLinearColor::Black;
	}

	const UDlgEditorSettings* Settings = GetDefault<UDlgEditorSettings>();
	if (IsSpeechNode())
	{
		if (IsVirtualParentNode())
		{
			return Settings->VirtualParentNodeColor;
		}

		return Settings->SpeechNodeColor;
	}

	if (IsSelectorNode())
	{
		if (IsSelectorFirstNode())
		{
			return Settings->SelectorFirstNodeColor;
		}

		if (IsSelectorRandomNode())
		{
			return Settings->SelectorRandomNodeColor;
		}

		// should not reach here, like never
		checkNoEntry();
	}

	if (IsSpeechSequenceNode())
	{
		return Settings->SpeechSequenceNodeColor;
	}

	if (IsEndNode())
	{
		return Settings->EndNodeColor;
	}

	return FLinearColor::Black;
}

bool UDialogueGraphNode::HasOutputConnectionToNode(const UEdGraphNode* TargetNode) const
{
	for (UDialogueGraphNode* ChildNode : GetChildNodes())
	{
		if (ChildNode == TargetNode)
		{
			return true;
		}
	}

	return false;;
}

void UDialogueGraphNode::RegisterListeners()
{
	Super::RegisterListeners();
	DialogueNode->OnDialogueNodePropertyChanged.AddUObject(this, &UDialogueGraphNode::OnDialogueNodePropertyChanged);
}
// End UDialogueGraphNode_Base interface
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin own functions
void UDialogueGraphNode::SetDialogueNodeDataChecked(int32 InIndex, UDlgNode* InNode)
{
	const UDlgDialogue* Dialogue = GetDialogue();
	checkf(Dialogue->GetNodes()[InIndex] == InNode, TEXT("The selected index = %d and with the Node provided does not match the Node from the Dialogue"), InIndex);

	SetDialogueNodeIndex(InIndex);
	SetDialogueNode(InNode);
}

void UDialogueGraphNode::SetEdgeTargetIndexAt(int32 EdgeIndex, int32 NewTargetIndex)
{
	check(NewTargetIndex > INDEX_NONE);
	const TArray<UDialogueGraphNode_Edge*> GraphNodeEdges = GetChildEdgeNodes();
	check(DialogueNode->GetNodeChildren().Num() == GraphNodeEdges.Num());
	check(GraphNodeEdges.IsValidIndex(EdgeIndex));

	DialogueNode->GetMutableNodeChildAt(EdgeIndex)->TargetIndex = NewTargetIndex;
	GraphNodeEdges[EdgeIndex]->SetDialogueEdgeTargetIndex(NewTargetIndex);
}

void UDialogueGraphNode::SetEdgeTextAt(int32 EdgeIndex, const FText& NewText)
{
	const TArray<UDialogueGraphNode_Edge*> GraphNodeEdges = GetChildEdgeNodes();
	check(DialogueNode->GetNodeChildren().Num() == GraphNodeEdges.Num());
	check(GraphNodeEdges.IsValidIndex(EdgeIndex));

	DialogueNode->GetMutableNodeChildAt(EdgeIndex)->Text = NewText;
	GraphNodeEdges[EdgeIndex]->SetDialogueEdgeText(NewText);
}

void UDialogueGraphNode::SetEdges(const TArray<FDlgEdge>& InEdges)
{
	const TArray<UDialogueGraphNode_Edge*> GraphNodeEdges = GetChildEdgeNodes();
	check(InEdges.Num() == GraphNodeEdges.Num());

	// Set the edges on the Dialogue
	DialogueNode->SetNodeChildren(InEdges);

	// Set the edges on the edge nodes
	for (int32 EdgeIndex = 0, EdgesNum = InEdges.Num(); EdgeIndex < EdgesNum; EdgeIndex++)
	{
		GraphNodeEdges[EdgeIndex]->SetDialogueEdge(InEdges[EdgeIndex]);
	}
}

void UDialogueGraphNode::ApplyCompilerWarnings()
{
	ClearCompilerMessage();

	// Is Orphan node
	if (!IsRootNode() && GetInputPin()->LinkedTo.Num() == 0)
	{
		SetCompilerWarningMessage(TEXT("Node has no input connections (orphan). It will not be accessible from anywhere"));
	}
	else if (DialogueNode->GetNodeOpenChildren_DEPRECATED().Num() > 0)
	{
		// Has open children :O
		SetCompilerWarningMessage(TEXT("Node has invalid (open) edges in its DialogueNode"));
	}
}

int32 UDialogueGraphNode::EstimateNodeWidth() const
{
	constexpr int32 EstimatedCharWidth = 6;
	// Check which is bigger, and use that
	const FString NodeTitle = GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	const FString NodeText = DialogueNode->GetNodeText().ToString();

	int32 Result;
	FString ResultFromString;
	if (NodeTitle.Len() > NodeText.Len())
	{
		Result = NodeTitle.Len() * EstimatedCharWidth;
		ResultFromString = NodeTitle;
	}
	else
	{
		Result = NodeText.Len() * EstimatedCharWidth;
		ResultFromString = NodeText;
	}

	if (const UFont* Font = GetDefault<UEditorEngine>()->EditorFont)
	{
		Result = Font->GetStringSize(*ResultFromString);
	}

	return Result;
}

void UDialogueGraphNode::CheckDialogueNodeIndexMatchesNode() const
{
#if DO_CHECK
	if (!IsRootNode())
	{
		const UDlgDialogue* Dialogue = GetDialogue();
		checkf(Dialogue->GetNodes()[NodeIndex] == DialogueNode, TEXT("The NodeIndex = %d with DialogueNode does not match the Node from the Dialogue at the same index"), NodeIndex);
	}
#endif
}

void UDialogueGraphNode::CheckDialogueNodeSyncWithGraphNode(bool bStrictCheck) const
{
#if DO_CHECK
	const UEdGraphPin* OutputPin = GetOutputPin();
	const TArray<FDlgEdge>& NodeEdges = DialogueNode->GetNodeChildren();
	const int32 EdgesNum = NodeEdges.Num();
	const int32 PinsLinkedToNum = OutputPin->LinkedTo.Num();
	check(PinsLinkedToNum == GetChildNodes().Num());
	checkf(EdgesNum == PinsLinkedToNum,
		TEXT("The node with OLD index = %d does not have the number of PinsLinkedToNum (%d) equal to the EdgesNum (%d) in it's FDlgNode (DialogueNode)"),
		NodeIndex, PinsLinkedToNum, EdgesNum);

	const int32 OpenEdgesNum = DialogueNode->GetNodeOpenChildren_DEPRECATED().Num();
	checkf(OpenEdgesNum == 0,
		TEXT("The node with OLD index = %d has open output pins (%d), this should not be allowed"),
		NodeIndex, OpenEdgesNum);

	checkf(DialogueNode->GetGraphNode() == this,
		TEXT("The node with OLD index = %d does not have the DialogueNode.GraphNode == this (graph node)"));

	// Check if Edges have the same openess as the output pins
	if (bStrictCheck)
	{
		for (int32 EdgeIndex = 0; EdgeIndex < EdgesNum; EdgeIndex++)
		{
			FString OutMessage;
			const FDlgEdge& Edge = NodeEdges[EdgeIndex];
			if (!DoesEdgeMatchEdgeIndex(Edge, EdgeIndex, OutMessage))
			{
				checkf(false,
					TEXT("The node with OLD index = %d does not have the dialogue edge at index = %d matching the Edge in the GraphNode. Error message = `%s`"),
					NodeIndex, EdgeIndex, *OutMessage);
			}
		}
	}
#endif
}

const TArray<UDialogueGraphNode*> UDialogueGraphNode::GetParentNodes() const
{
	// (input pin) ParentNode (output pin) -> (input pin) EdgeNode aka ParentEdgeConnection (output pin) -> (input pin) ThisNode (output pin)
	TArray<UDialogueGraphNode*> ParentNodes;
	for (const UDialogueGraphNode_Edge* ParentEdgeConnection : GetParentEdgeNodes())
	{
		ParentNodes.Add(ParentEdgeConnection->GetParentNode());
	}

	return ParentNodes;
}

const TArray<UDialogueGraphNode*> UDialogueGraphNode::GetChildNodes() const
{
	// (input pin) ThisNode (output pin) -> (input pin) EdgeNode aka ChildEdgeConnection (output pin) -> (input pin) ChildNode (output pin)
	TArray<UDialogueGraphNode*> ChildNodes;
	for (const UDialogueGraphNode_Edge* ChildEdgeConnection : GetChildEdgeNodes())
	{
		ChildNodes.Add(ChildEdgeConnection->GetChildNode());
	}

	return ChildNodes;
}

const TArray<UDialogueGraphNode_Edge*> UDialogueGraphNode::GetParentEdgeNodes() const
{
	// (input pin) ParentNode (output pin) -> (input pin) EdgeNode aka ParentEdgeConnection (EdgeOutputPin) -> (input pin) ThisNode (output pin)
	TArray<UDialogueGraphNode_Edge*> ParentEdgeNodes;
	for (const UEdGraphPin* EdgeOutputPin : GetInputPin()->LinkedTo)
	{
		UDialogueGraphNode_Edge* ParentEdgeConnection = CastChecked<UDialogueGraphNode_Edge>(EdgeOutputPin->GetOwningNode());
		check(ParentEdgeConnection->GetChildNode() == this);
		ParentEdgeNodes.Add(ParentEdgeConnection);
	}

	return ParentEdgeNodes;
}

const TArray<UDialogueGraphNode_Edge*> UDialogueGraphNode::GetChildEdgeNodes(const bool bCheckParent /*= true*/) const
{
	// (input pin) ThisNode (output pin) -> (EdgeInputPin) EdgeNode aka ChildEdgeConnection (output pin) -> (input pin) ChildNode (output pin)
	TArray<UDialogueGraphNode_Edge*> ChildEdgeNodes;
	for (const UEdGraphPin* EdgeInputPin : GetOutputPin()->LinkedTo)
	{
		UDialogueGraphNode_Edge* ChildEdgeConnection = CastChecked<UDialogueGraphNode_Edge>(EdgeInputPin->GetOwningNode());

#if DO_CHECK
		if (bCheckParent)
		{
			check(ChildEdgeConnection->GetParentNode() == this);
		}
#endif

		ChildEdgeNodes.Add(ChildEdgeConnection);
	}

	return ChildEdgeNodes;
}

struct FCompareNodeXLocation
{
	FORCEINLINE bool operator()(const TPair<UEdGraphPin*, FDlgEdge>& A, const TPair<UEdGraphPin*, FDlgEdge>& B) const
	{
		return A.Key->GetOwningNode()->NodePosX < B.Key->GetOwningNode()->NodePosX;
	}
};

void UDialogueGraphNode::SortChildrenBasedOnXLocation()
{
	// Holds an array of synced pairs, each pair corresponds to a linked to output pin and corresponding dialogue edge
	TArray<TPair<UEdGraphPin*, FDlgEdge>> SyncedArray;

	UEdGraphPin* OutputPin = GetOutputPin();
	const TArray<UEdGraphPin*> ChildPins = OutputPin->LinkedTo;
	const TArray<FDlgEdge>& ChildDialogueNodeEdges = DialogueNode->GetNodeChildren();
	check(ChildPins.Num() == ChildDialogueNodeEdges.Num());

	// Step 1. Construct the synced array
	const int32 ChildrenNum = ChildPins.Num();
	SyncedArray.Reserve(ChildrenNum);
	for (int32 ChildIndex = 0; ChildIndex < ChildrenNum; ChildIndex++)
	{
		SyncedArray.Emplace(ChildPins[ChildIndex], ChildDialogueNodeEdges[ChildIndex]);
	}

	// Step 2. Sort the synced array
	SyncedArray.Sort(FCompareNodeXLocation());

	// Step 3. Reconstruct the output pins/edges from the sorted synced array
	OutputPin->LinkedTo.Empty();
	DialogueNode->RemoveAllChildren();
	for (const TPair<UEdGraphPin*, FDlgEdge>& SyncedPair : SyncedArray)
	{
		OutputPin->LinkedTo.Add(SyncedPair.Key);
		DialogueNode->AddNodeChild(SyncedPair.Value);
	}
}

void UDialogueGraphNode::OnDialogueNodePropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent, int32 EdgeIndexChanged)
{
	if (PropertyChangedEvent.Property == nullptr)
	{
		return;
	}

	// Keep in sync the Edge Graph Node with the modified Edge from the Details Panel of the Parent Node
	const TArray<FDlgEdge>& DialogueEdges = DialogueNode->GetNodeChildren();
	if (EdgeIndexChanged != INDEX_NONE)
	{
		// We know what edge changed, update that, most likelye and add/insert/modify
		const TArray<UDialogueGraphNode_Edge*> ChildEdgeNodes = GetChildEdgeNodes();
		check(ChildEdgeNodes.IsValidIndex(EdgeIndexChanged))

		// Update the proxy edge node with the new edge
		ChildEdgeNodes[EdgeIndexChanged]->SetDialogueEdge(DialogueEdges[EdgeIndexChanged]);
	}
	else
	{
		// We do not know that edge, update all, most likely remove
		const TArray<UDialogueGraphNode_Edge*> ChildEdgeNodes = GetChildEdgeNodes();
		for (int32 EdgeIndex = 0, EdgesNum = ChildEdgeNodes.Num(); EdgeIndex < EdgesNum; EdgeIndex++)
		{
			// Update the proxy edge node with the new edge
			ChildEdgeNodes[EdgeIndex]->SetDialogueEdge(DialogueEdges[EdgeIndex]);
		}
	}
}

void UDialogueGraphNode::ResetDialogueNodeOwner()
{
	if (!DialogueNode)
	{
		return;
	}

	UDlgDialogue* Dialogue = GetDialogue();

	// Ensures DialogueNode is owned by the Dialogue
	if (DialogueNode->GetOuter() != Dialogue)
	{
		DialogueNode->Rename(nullptr, Dialogue, REN_DontCreateRedirectors);
	}

	// Set up the back pointer for newly created dialogue node nodes
	DialogueNode->SetGraphNode(this);
	DialogueNode->SetFlags(RF_Transactional);
}

const FDiffNodeEdgeLinkedToPinResult UDialogueGraphNode::FindDifferenceBetweenNodeEdgesAndLinkedToPins() const
{
	FDiffNodeEdgeLinkedToPinResult Result;
	const UEdGraphPin* OutputPin = GetOutputPin();
	const TArray<FDlgEdge>& NodeEdges = GetDialogueNode().GetNodeChildren();
	const int32 EdgesNum = NodeEdges.Num();
	const int32 LinkedToNum = OutputPin->LinkedTo.Num();
	const bool bAreLengthsEqual = EdgesNum == LinkedToNum;

	// If EdgesNum > LinkedToNum, we are finding the (only) edge that is different from the linked pins
	// If LinkedToNum > EdgesNum, we are finding the (only) linked pin that is different from the edges
	if (!bAreLengthsEqual && FMath::Abs(EdgesNum - LinkedToNum) != 1)
	{
		UE_LOG(LogDlgSystemEditor, Error,
			TEXT("FindDifferenceBetweenNodeEdgesAndLinkedToPins: EdgesNum (%d) != LinkedToNum (%d), but function was called in the wrong context"),
			EdgesNum, LinkedToNum);
		Result.Type = FDiffNodeEdgeLinkedToPinResult::EDiffType::NOT_SUPPORTED;
		return Result;
	}

	// Try to find difference in common areas of both arrays
	for (int32 Index = 0; Index < EdgesNum && Index < LinkedToNum; Index++)
	{
		const FDlgEdge& Edge = NodeEdges[Index];
		if (!DoesEdgeMatchEdgeIndex(Edge, Index, Result.Message))
		{
			Result.Index = Index;
			Result.Type = FDiffNodeEdgeLinkedToPinResult::EDiffType::EDGE_NOT_MATCHING_INDEX;
			// Assume next Edge matches index
#if DO_CHECK
			if (Index + 1 < EdgesNum && Index + 1 < LinkedToNum)
			{
				check(DoesEdgeMatchEdgeIndex(NodeEdges[Index + 1], Index, Result.Message));
			}
#endif

			return Result;
		}
	}

	// Did not find any result in the common indices area :(
	if (!bAreLengthsEqual && Result.Index == INDEX_NONE)
	{
		if (LinkedToNum > EdgesNum)
		{
			// Because LinkedToNum > EdgesNum, it means that the last edge was removed OR a pin connection was added
			Result.Index = LinkedToNum - 1;
			Result.Type = FDiffNodeEdgeLinkedToPinResult::EDiffType::LENGTH_MISMATCH_ONE_MORE_PIN_CONNECTION;
		}
		else
		{
			check(EdgesNum > LinkedToNum);
			// Because EdgesNum > LinkedToNum, it means that the last linked to pin connection was removed OR an edge was added
			Result.Index = EdgesNum - 1;
			Result.Type = FDiffNodeEdgeLinkedToPinResult::EDiffType::LENGTH_MISMATCH_ONE_MORE_EDGE;
		}
	}

	return Result;
}

bool UDialogueGraphNode::DoesEdgeMatchEdgeIndex(const FDlgEdge& Edge, int32 EdgeIndex, FString& OutMessage) const
{
	// (input pin) ThisNode (ThisOutputPin) -> (EdgeInputPin) ChildEdgeConnection (output pin) -> (input pin) ChildNode (output pin)
	check(Edge.IsValid());
	UEdGraphPin* ThisOutputPin = GetOutputPin();
	if (!ThisOutputPin->LinkedTo.IsValidIndex(EdgeIndex))
	{
		OutMessage = FString::Printf(TEXT("The provided EdgeIndex = %d is not a valid index in the ThisOutputPin->LinkedTo Array"), EdgeIndex);
		return false;
	}

	// Check if Edge.TargetIndex matches the NodeIndex of the ChildNode
	const UEdGraphPin* EdgeInputPin = ThisOutputPin->LinkedTo[EdgeIndex];
	check(EdgeInputPin->Direction == EGPD_Input);

	// Walk to child
	const UDialogueGraphNode_Edge* ChildEdgeConnection = CastChecked<UDialogueGraphNode_Edge>(EdgeInputPin->GetOwningNode());
	const UDialogueGraphNode* ChildNode = ChildEdgeConnection->GetChildNode();
	check(ChildNode != this);

	// Edge differs :(
	if (Edge != ChildEdgeConnection->GetDialogueEdge())
	{
		OutMessage = TEXT("The provided Edge does not match the ChildEdgeConnection.Edge");
		return false;
	}

	// Target node Index differs :(
	if (Edge.TargetIndex != ChildNode->GetDialogueNodeIndex())
	{
		OutMessage = FString::Printf(TEXT("The provided Edge.TargetIndex = %d is DIFFERENT from the ChildNode.NodeIndex = %d"),
				Edge.TargetIndex, ChildNode->GetDialogueNodeIndex());
		return false;
	}

	return true;
}
// End own functions
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
