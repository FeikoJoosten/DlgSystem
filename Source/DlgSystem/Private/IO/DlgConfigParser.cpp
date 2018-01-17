// Copyright 2017-2018 Csaba Molnar, Daniel Butum
#include "IO/DlgConfigParser.h"
#include "LogMacros.h"
#include "FileHelper.h"
#include "Paths.h"
#include "UnrealType.h"
#include "EnumProperty.h"
#include "UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogDlgConfigParser);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
DlgConfigParser::DlgConfigParser(const FString& InPreTag) :
	PreTag(InPreTag)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
DlgConfigParser::DlgConfigParser(const FString& FilePath, const FString& InPreTag) :
	PreTag(InPreTag)
{
	InitializeParser(FilePath);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgConfigParser::InitializeParser(const FString& FilePath)
{
	FileName = "";
	String = "";
	From = 0;
	Len = 0;
	bHasValidWord = false;

	if (!FFileHelper::LoadFileToString(String, *FilePath))
	{
		UE_LOG(LogDlgConfigParser, Error, TEXT("Failed to load config file %s"), *FilePath)
	}
	else
	{
		// find first word
		FindNextWord();
	}

	FileName = FPaths::GetBaseFilename(FilePath, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgConfigParser::InitializeParserFromString(const FString& Text)
{
	FileName = "";
	String = Text;
	From = 0;
	Len = 0;
	bHasValidWord = false;
	FindNextWord();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgConfigParser::ReadAllProperty(const UStruct* ReferenceClass, void* TargetObject, UObject* DefaultObjectOuter)
{
	while (ReadProperty(ReferenceClass, TargetObject, DefaultObjectOuter));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::ReadProperty(const UStruct* ReferenceClass, void* TargetObject, UObject* DefaultObjectOuter)
{
	if (!bHasValidWord)
	{
		return false;
	}

	check(From < String.Len());

	const FString PropertyName = GetActiveWord();
	UProperty* PropertyBase = ReferenceClass->FindPropertyByName(*PropertyName);
	if (PropertyBase != nullptr)
	{
		// check primitive types and enums
		if (TryToReadPrimitiveProperty(TargetObject, PropertyBase) || TryToReadEnum(TargetObject, PropertyBase))
		{
			return true;
		}

		// check <MAP>
		UMapProperty* MapProperty = Cast<UMapProperty>(PropertyBase);
		if (MapProperty != nullptr)
		{
			return ReadMap(TargetObject, *MapProperty, DefaultObjectOuter);
		}

		// check <SET>
		USetProperty* SetProperty = Cast<USetProperty>(PropertyBase);
		if (SetProperty != nullptr)
		{
			return ReadSet(TargetObject, *SetProperty, DefaultObjectOuter);
		}
	}

	UProperty* ComplexPropBase = ReferenceClass->FindPropertyByName(*PropertyName);

	// struct
	UStructProperty* StructProperty = SmartCastProperty<UStructProperty>(ComplexPropBase);
	if (StructProperty != nullptr)
	{
		return ReadComplexProperty<UStructProperty>(TargetObject,
													ComplexPropBase,
													StructProperty->Struct,
													[](void* Ptr, const UClass*, UObject*) { return Ptr; },
													DefaultObjectOuter);
	}

	// check complex object - type name has to be here as well (dynamic array)
	const FString TypeName = PreTag + PropertyName;
	if (!FindNextWord("block name"))
	{
		return false;
	}

	const bool bLoadByRef = IsNextWordString();
	const FString VariableName = GetActiveWord();

	// check if it is stored as reference
	if (bLoadByRef)
	{
		// sanity check: if it is not an uobject** we should not try to write it!
		if (SmartCastProperty<UObjectProperty>(ComplexPropBase) == nullptr)
		{
			return false;
		}

		void* TargetPtr = ComplexPropBase->template ContainerPtrToValuePtr<void>(TargetObject);
		*reinterpret_cast<UObject**>(TargetPtr) = StaticLoadObject(UObject::StaticClass(), NULL, *VariableName);
		FindNextWord();
		return true;
	}

	ComplexPropBase = ReferenceClass->FindPropertyByName(*VariableName);

	// UObject
	UObjectProperty* ObjectProp = SmartCastProperty<UObjectProperty>(ComplexPropBase);
	if (ObjectProp != nullptr)
	{
		const UClass* Class = SmartGetPropertyClass(ComplexPropBase, TypeName);
		if (Class == nullptr)
		{
			return false;
		}
		auto ObjectInitializer = std::bind(&DlgConfigParser::OnInitObject, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
		return ReadComplexProperty<UObjectProperty>(TargetObject, ComplexPropBase, Class, ObjectInitializer, DefaultObjectOuter);
	}

	UE_LOG(LogDlgConfigParser, Warning, TEXT("Invalid token %s in script %s (line: %d) (Property expected)"),
		   *GetActiveWord(), *FileName, GetActiveLineNumber());
	FindNextWord();
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::ReadPurePropertyBlock(void* TargetObject, const UStruct* ReferenceClass, bool bBlockStartAlreadyRead, UObject* Outer)
{
	if (!bBlockStartAlreadyRead && !FindNextWordAndCheckIfBlockStart(ReferenceClass->GetName()))
	{
		return false;
	}

	// parse precondition properties
	FindNextWord();
	while (!CheckIfBlockEnd(ReferenceClass->GetName()))
	{
		if (!bHasValidWord)
		{
			return false;
		}

		ReadProperty(ReferenceClass, TargetObject, Outer);
	}

	FindNextWord();
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::GetActiveWordAsFloat(float& FloatValue) const
{
	if (!HasValidWord())
	{
		return false;
	}

	const FString FloatString = String.Mid(From, Len);
	if (FloatString.Len() == 0 || !FloatString.IsNumeric())
	{
		return false;
	}

	FloatValue = FCString::Atof(*FloatString);
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FString DlgConfigParser::ConstructConfigFile(const UStruct* ReferenceType, void* SourceObject)
{
	FString String;
	ConstructConfigFileInternal(ReferenceType, 0, SourceObject, String);
	return String;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::IsNextWordString() const
{
	int32 Index = From + Len;
	// Skip whitespaces
	while (Index < String.Len() && FChar::IsWhitespace(String[Index]))
	{
		Index++;
	}

	return (Index < String.Len() && String[Index] == '"');
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::IsActualWordString() const
{
	int32 Index = From - 1;
	return (String.IsValidIndex(Index) && String[Index] == '"');
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::FindNextWord()
{
	From += Len;

	// Skip " (aka the open of string)
	if (bActiveIsString)
	{
		From++;
	}
	bActiveIsString = false;

	// Skip whitespaces
	while (From < String.Len() && FChar::IsWhitespace(String[From]))
	{
		From++;
	}

	// Oh noeeeees
	if (From >= String.Len())
	{
		bHasValidWord = false;
		return false;
	}

	// Handle special string case - read everything between two "
	if (String[From] == '"')
	{
		Len = 1;
		bActiveIsString = true;
		// Find the closing "
		while (From + Len < String.Len() && String[From + Len] != '"')
		{
			Len++;
		}

		// Do not include the "" in the range
		From += 1;
		Len -= 1;

		// Something very bad happened
		if (Len <= 0)
		{
			bHasValidWord = false;
			return false;
		}

		bHasValidWord = true;
		return true;
	}

	Len = 0;
	// Is block begin/end
	if (String[From] == '{' || String[From] == '}')
	{
		Len = 1;
	}
	else
	{
		// Count until we reach a whitespace char OR EOF
		while (From + Len < String.Len() && !FChar::IsWhitespace(String[From + Len]))
		{
			Len++;
		}
	}

	// Skip comments //
	if (Len > 1 && String[From] == '/' && String[From + 1] == '/')
	{
		// Advance past this line
		while (From < String.Len() && String[From] != '\n' && String[From] != '\r')
		{
			From++;
		}

		// Use recursion to go to the next line
		Len = 0;
		return FindNextWord();
	}

	// Phew, valid word
	bHasValidWord = true;
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::FindNextWord(const FString& ExpectedStuff)
{
	const bool bNotEof = FindNextWord();
	if (!bNotEof)
	{
		UE_LOG(LogDlgConfigParser, Warning, TEXT("Unexpected end of file while reading %s (expected: %s)"), *FileName, *ExpectedStuff);
	}

	return bNotEof;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::FindNextWordAndCheckIfBlockStart(const FString& BlockName)
{
	if (!FindNextWord() || !CompareToActiveWord("{"))
	{
		UE_LOG(LogDlgConfigParser, Warning, TEXT("Block start signal expected but not found for %s block in script %s (line: %d)"),
											*BlockName, *FileName, GetActiveLineNumber());
		return false;
	}
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::FindNextWordAndCheckIfBlockEnd(const FString& BlockName)
{
	if (!FindNextWord())
	{
		UE_LOG(LogDlgConfigParser, Warning, TEXT("End of file found but block %s is not yet closed in script %s (line: %d)"),
											*BlockName, *FileName, GetActiveLineNumber());
		return false;
	}
	return Len == 1 && String[From] == '}';
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::CheckIfBlockEnd(const FString& BlockName)
{
	if (!bHasValidWord)
	{
		UE_LOG(LogDlgConfigParser, Warning, TEXT("End of file found but block %s is not yet closed in script %s (line: %d)"),
											*BlockName, *FileName, GetActiveLineNumber());
		return false;
	}
	return Len == 1 && String[From] == '}';
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::CompareToActiveWord(const FString& StringToCompare) const
{
	// Length differs?
	if (!bHasValidWord || StringToCompare.Len() != Len)
	{
		return false;
	}

	// Content differs?
	const TCHAR* SubStr = &String[From];
	for (int32 i = 0; i < Len; ++i)
	{
		if (SubStr[i] != StringToCompare[i])
		{
			return false;
		}
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int32 DlgConfigParser::GetActiveLineNumber() const
{
	if (!bHasValidWord)
	{
		return INDEX_NONE;
	}

	int32 LineCount = 1;
	for (int32 i = 0; i < String.Len() && i < From; ++i)
	{
		switch (String[i])
		{
			case '\r':
				// let's handle '\r\n too
				if (i + 1 < String.Len() && String[i + 1] == '\n')
					++i;
			case '\n':
				++LineCount;
				break;

			default:
				break;
		}
	}

	return LineCount;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgConfigParser::ConstructConfigFileInternal(const UStruct* ReferenceType, const int32 TabCount, void* SourceObject, FString& OutString)
{
	for (UField* Field = ReferenceType->Children; Field != nullptr; Field = Field->Next)
	{
		UBoolProperty* BoolProp = Cast<UBoolProperty>(Field);
		if (BoolProp != nullptr)
		{
			OutString += BoolProp->GetName() + " " + (BoolProp->GetPropertyValue_InContainer(SourceObject) ? "True\n" : "False\n");
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::TryToReadPrimitiveProperty(void* TargetObject, UProperty* PropertyBase)
{
	if (ReadPrimitiveProperty<bool, UBoolProperty>(TargetObject, PropertyBase, std::bind(&DlgConfigParser::GetAsBool, this), "Bool", false))
	{
		return true;
	}
	if (ReadPrimitiveProperty<float, UFloatProperty>(TargetObject, PropertyBase, std::bind(&DlgConfigParser::GetAsFloat, this), "float", false))
	{
		return true;
	}
	if (ReadPrimitiveProperty<int32, UIntProperty>(TargetObject, PropertyBase, std::bind(&DlgConfigParser::GetAsInt, this), "int32", false))
	{
		return true;
	}
	if (ReadPrimitiveProperty<FName, UNameProperty>(TargetObject, PropertyBase, std::bind(&DlgConfigParser::GetAsName, this), "FName", false))
	{
		return true;
	}
	if (ReadPrimitiveProperty<FString, UStrProperty>(TargetObject, PropertyBase, std::bind(&DlgConfigParser::GetAsString, this), "FString", true))
	{
		return true;
	}
	if (ReadPrimitiveProperty<FText, UTextProperty>(TargetObject, PropertyBase, std::bind(&DlgConfigParser::GetAsText, this), "FText", true))
	{
		return true;
	}

	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::TryToReadEnum(void* Target, UProperty* PropertyBase)
{
	auto OnGetAsEnum = [this, &PropertyBase]() -> uint8
	{
		FName Value = NAME_None;
		if (Len <= 0)
		{
			OnInvalidValue("FName");
		}
		else
		{
			Value = FName(*String.Mid(From, Len));
		}

		UEnumProperty* Prop = SmartCastProperty<UEnumProperty>(PropertyBase);
		if (Prop == nullptr || Prop->GetEnum() == nullptr)
		{
			return 0;
		}

		check(Cast<UByteProperty>(Prop->GetUnderlyingProperty()));
		return uint8(Prop->GetEnum()->GetIndexByName(Value));
	};
	// enum can't be pure array atm!!!
	UEnumProperty* EnumProp = Cast<UEnumProperty>(PropertyBase);
	if (EnumProp != nullptr)
	{
		FindNextWord();
		if (bHasValidWord)
		{
			// ContainerPtrToValuePtr has to be called on the enum, not on the underlying prop!!!
			void* Value = EnumProp->ContainerPtrToValuePtr<uint8>(Target);
			EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(Value, static_cast<int64>(OnGetAsEnum()));
		}
		else
		{
			UE_LOG(LogDlgConfigParser, Warning, TEXT("Unexpected end of file while enum value was expected (config %s)"), *FileName);
		}

		FindNextWord();
		return true;
	}

	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::ReadSet(void* TargetObject, USetProperty& Property, UObject* DefaultObjectOuter)
{
	FScriptSetHelper Helper(&Property, Property.ContainerPtrToValuePtr<uint8>(TargetObject));
	Helper.EmptyElements();

	if (!FindNextWordAndCheckIfBlockStart("Set block"))
	{
		return false;
	}

	while (!FindNextWordAndCheckIfBlockEnd("Set block"))
	{
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		bool bDone = false;
		uint8* ElementPtr = Helper.GetElementPtr(Index);
		if (Cast<UBoolProperty>(Helper.ElementProp)) { *(bool*)ElementPtr = GetAsBool();		bDone = true; }
		else if (Cast<UFloatProperty>(Helper.ElementProp)) { *(float*)ElementPtr = GetAsFloat();		bDone = true; }
		else if (Cast<UIntProperty>(Helper.ElementProp)) { *(int32*)ElementPtr = GetAsInt();		bDone = true; }
		else if (Cast<UNameProperty>(Helper.ElementProp)) { *(FName*)ElementPtr = GetAsName();		bDone = true; }
		else if (Cast<UStrProperty>(Helper.ElementProp)) { *(FString*)ElementPtr = GetAsString();	bDone = true; }
		else if (Cast<UTextProperty>(Helper.ElementProp)) { *(FText*)ElementPtr = GetAsText();		bDone = true; }
		// else if (Cast<UByteProperty>(Helper.ElementProp))	{ *(uint8*)ElementPtr	= OnGetAsEnum();	bDone = true; } // would not work, check enum above

		if (!bDone)
		{
			UE_LOG(LogDlgConfigParser, Warning, TEXT("Unsupported set element type %s in script %s(:%d)"),
				*Helper.ElementProp->GetName(), *FileName, GetActiveLineNumber());
			return false;
		}
		Helper.Rehash();
	}
	FindNextWord();
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::ReadMap(void* TargetObject, UMapProperty& Property, UObject* DefaultObjectOuter)
{
	FScriptMapHelper Helper(&Property, Property.ContainerPtrToValuePtr<uint8>(TargetObject));
	Helper.EmptyValues();

	if (!FindNextWordAndCheckIfBlockStart("Map block") || !FindNextWord("map entry"))
	{
		return false;
	}

	while (!CheckIfBlockEnd("Map block"))
	{
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		void* Ptrs[] = { Helper.GetKeyPtr(Index), Helper.GetValuePtr(Index) };
		UProperty* Props[] = { Helper.KeyProp, Helper.ValueProp };
		bool bDone = false;

		for (int32 i = 0; i < 2; ++i)
		{
			if (Cast<UBoolProperty>(Props[i]))			{ *(bool*)Ptrs[i]	 = GetAsBool();		bDone = true; }
			else if (Cast<UFloatProperty>(Props[i]))	{ *(float*)Ptrs[i]	 = GetAsFloat();	bDone = true; }
			else if (Cast<UIntProperty>(Props[i]))		{ *(int32*)Ptrs[i]	 = GetAsInt();		bDone = true; }
			else if (Cast<UNameProperty>(Props[i]))		{ *(FName*)Ptrs[i]	 = GetAsName();		bDone = true; }
			else if (Cast<UStrProperty>(Props[i]))		{ *(FString*)Ptrs[i] = GetAsString();	bDone = true; }
			else if (Cast<UTextProperty>(Props[i]))		{ *(FText*)Ptrs[i]   = GetAsText();		bDone = true; }
			// else if (Cast<UByteProperty>(Props[i]))		{ *(uint8*)Ptrs[i]	 = OnGetAsEnum();	bDone = true; } // would not work, check enum above

			if (i == 0)
			{
				if (!bDone)
				{
					UE_LOG(LogDlgConfigParser, Warning, TEXT("Invalid map key type %s in script %s(:%d)"),
														*Helper.KeyProp->GetName(), *FileName, GetActiveLineNumber());
					return false;
				}
				bDone = false;
				if (!FindNextWord("Map Value"))
				{
					return false;
				}

			}
			else
			{
				if (bDone && !FindNextWord("New map key or map end"))
				{
					return false;
				}
			}
		}

		if (!bDone)
		{
			UStructProperty* StructVal = Cast<UStructProperty>(Helper.ValueProp);
			if (StructVal != nullptr)
			{
				if (!CompareToActiveWord("{"))
				{
					UE_LOG(LogDlgConfigParser, Warning, TEXT("Syntax error: missing struct block start '{' in script %s(:%d)"),
														*FileName, GetActiveLineNumber());
					return false;
				}
				if (!ReadPurePropertyBlock(Helper.GetValuePtr(Index), StructVal->Struct, true, DefaultObjectOuter))
				{
					return false;
				}
			}
		}
		Helper.Rehash();
	} // while (!CheckIfBlockEnd("Map block"))

	FindNextWord();
	return true;
}

void* DlgConfigParser::OnInitObject(void* ValuePtr, const UClass* ChildClass, UObject* OuterInit)
{
	if (ChildClass != nullptr)
	{
		UObject** Value = (UObject**)ValuePtr;
		(*Value) = CreateDefaultUObject(ChildClass, OuterInit);
		return (*Value);
	}
	UE_LOG(LogDlgConfigParser, Warning, TEXT("OnInitValue called without class!"));
	return nullptr;
}

/** gets the UClass from an UObject or from an array of UObjects */
const UClass* DlgConfigParser::SmartGetPropertyClass(UProperty* Property, const FString& TypeName)
{
	UObjectProperty* ObjectProperty = SmartCastProperty<UObjectProperty>(Property);
	check(ObjectProperty != nullptr);

	const UClass* Class = nullptr;
	if (Cast<UArrayProperty>(Property) != nullptr)
	{
		Class = ObjectProperty->PropertyClass;
	}
	else
	{
		Class = GetChildClassFromName(ObjectProperty->PropertyClass, TypeName);
	}

	if (Class == nullptr)
	{
		UE_LOG(LogDlgConfigParser, Warning, TEXT("Could not find class %s for %s in config %s (:%d)"),
			   *TypeName, *ObjectProperty->GetName(), *FileName, GetActiveLineNumber());
	}

	return Class;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgConfigParser::OnInvalidValue(const FString& PropType) const
{
	if (bHasValidWord)
	{
		const int32 LineNumber = GetActiveLineNumber();
		UE_LOG(LogDlgConfigParser, Warning, TEXT("Invalid %s property value %s in script %s (line %d)"),
			*PropType, *String.Mid(From, Len), *FileName, LineNumber);
	}
	else
		UE_LOG(LogDlgConfigParser, Warning, TEXT("Unexepcted end of file while expecting %s value in script %s"), *PropType, *FileName);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgConfigParser::GetAsBool() const
{
	bool bValue = false;
	if (CompareToActiveWord("True"))
		bValue = true;
	else if (!CompareToActiveWord("False"))
		OnInvalidValue("Bool");
	return bValue;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
float DlgConfigParser::GetAsFloat() const
{
	float Value = 0.0f;
	if (!GetActiveWordAsFloat(Value))
		OnInvalidValue("Float");
	return Value;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int32 DlgConfigParser::GetAsInt() const
{
	int32 Value = 0;
	const FString IntString = String.Mid(From, Len);
	if (IntString.Len() == 0 || (!IntString.IsNumeric()))
		OnInvalidValue("int32");
	else
		Value = FCString::Atoi(*IntString);
	return Value;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FName DlgConfigParser::GetAsName() const
{
	FName Value = NAME_None;
	if (Len <= 0)
		OnInvalidValue("FName");
	else
		Value = FName(*String.Mid(From, Len));
	return Value;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FString DlgConfigParser::GetAsString() const
{
	if (Len > 0)
		return String.Mid(From, Len);

	return "";
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FText DlgConfigParser::GetAsText() const
{
	FString Input;
	if (Len > 0)
		Input = String.Mid(From, Len);

	return FText::FromString(Input);
}