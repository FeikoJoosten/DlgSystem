// Copyright 2017-2018 Csaba Molnar, Daniel Butum
#pragma once

#include "CoreTypes.h"
#include "LogMacros.h"
#include "JsonValue.h"
#include "JsonObject.h"

#include "IDlgParser.h"

DECLARE_LOG_CATEGORY_EXTERN(LogDlgJsonParser, Log, All);


/**
 * @brief The DlgJsonParser class mostly adapted for Dialogues, copied from FJsonObjectConverter
 * See IDlgParser for properties and METADATA specifiers.
 */
class DLGSYSTEM_API DlgJsonParser : public IDlgParser
{
	/**
	 * Call Order and possible calls:
	 *  - DlgJsonParser
	 *		- InitializeParser
	 *			- JsonObjectStringToUStruct
	 *				- JsonObjectToUStruct
	 *					- JsonAttributesToUStruct
	 *						- JsonValueToUProperty
	 *							- ConvertScalarJsonValueToUProperty
	 *								- JsonValueToUProperty
	 *								- JsonObjectToUStruct
	 */

public:
	DlgJsonParser() {}

	DlgJsonParser(const FString& FilePath)
	{
		InitializeParser(FilePath);
	}

	// IDlgParser Interface
	void InitializeParser(const FString& FilePath) override;
	bool IsValidFile() const override { return bIsValidFile; }
	void ReadAllProperty(const UStruct* ReferenceClass, void* TargetObject, UObject* DefaultObjectOuter = nullptr) override;

private: // JSON -> UStruct

	/**
	 * Convert JSON to property, assuming either the property is not an array or the value is an individual array element
	 * Used by JsonValueToUProperty
	 */
	bool ConvertScalarJsonValueToUProperty(TSharedPtr<FJsonValue> JsonValue, UProperty* Property, void* OutValue);

	/**
	 * Converts a single JsonValue to the corresponding UProperty (this may recurse if the property is a UStruct for instance).
	 *
	 * @param JsonValue The value to assign to this property
	 * @param Property The UProperty definition of the property we're setting.
	 * @param OutValue Pointer to the property instance to be modified.
	 *
	 * @return False if the property failed to serialize
	 */
	bool JsonValueToUProperty(const TSharedPtr<FJsonValue> JsonValue, UProperty* Property, void* OutValue);

	/**
	 * Converts a set of json attributes (possibly from within a JsonObject) to a UStruct, using importText
	 *
	 * @param JsonAttributes 	Json Object to copy data out of
	 * @param StructDefinition  UStruct definition that is looked over for properties
	 * @param OutPtr 			The Pointer instance to copy in to
	 *
	 * @return False if any properties matched but failed to deserialize
	 */
	bool JsonAttributesToUStruct(const TMap<FString, TSharedPtr<FJsonValue>>& JsonAttributes,
								const UStruct* StructDefinition, void* OutPtr);

	/**
	 * Converts from a Json Object to a UStruct, using importText
	 *
	 * @param JsonObject 		Json Object to copy data out of
	 * @param StructDefinition  UStruct definition that is looked over for properties
	 * @param OutPtr 			The Pointer instance to copy in to
	 *
	 * @return False if any properties matched but failed to deserialize
	 */
	bool JsonObjectToUStruct(const TSharedRef<FJsonObject>& JsonObject, const UStruct* StructDefinition, void* OutPtr)
	{
		return JsonAttributesToUStruct(JsonObject->Values, StructDefinition, OutPtr);
	}

	/**
	 * Converts from a json string containing an object to a UStruct
	 *
	 * @param OutStruct The UStruct instance to copy in to
	 *
	 * @return False if any properties matched but failed to deserialize
	 */
	bool JsonObjectStringToUStruct(const UStruct* StructDefinition, void* TargetPtr);

private:
	FString JsonString;
	FString FileName;
	bool bIsValidFile = false;

	/** The default object outer used when creating new objects when using NewObject.  */
	UObject* DefaultObjectOuter = nullptr;

	/** Only properties that have these flags will be read. */
	static constexpr int64 CheckFlags = ~CPF_ParmFlags;
};
