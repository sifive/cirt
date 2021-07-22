//===- FIRParser.cpp - .fir to FIRRTL dialect parser ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This implements a .fir file parser.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "FIRAnnotations.h"
#include "FIRLexer.h"
#include "circt/Dialect/FIRRTL/FIRRTLAttributes.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Threading.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Translation.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace circt;
using namespace firrtl;

using llvm::SMLoc;
using llvm::SourceMgr;
using mlir::FailureOr;
using mlir::LocationAttr;

namespace json = llvm::json;

/// Return true if this is a useless temporary name produced by FIRRTL.  We
/// drop these as they don't convey semantic meaning.
static bool isUselessName(StringRef name) {
  // Ignore _T and _T_123
  if (name.startswith("_T")) {
    if (name.size() == 2)
      return true;
    return name.size() > 3 && name[2] == '_' && llvm::isDigit(name[3]);
  }

  // Ignore _GEN and _GEN_123, these are produced by Namespace.scala.
  if (name.startswith("_GEN")) {
    if (name.size() == 4)
      return true;
    return name.size() > 5 && name[4] == '_' && llvm::isDigit(name[5]);
  }
  return false;
}

/// If the specified name is a useless temporary name produced by FIRRTL, return
/// an empty string to ignore it.  Otherwise, return the argument unmodified.
static StringRef filterUselessName(StringRef name) {
  return isUselessName(name) ? "" : name;
}

/// Get an annotation with a class name field.
static DictionaryAttr getAnnotationOfClass(MLIRContext *context,
                                           StringRef classString) {
  auto id = NamedAttribute(Identifier::get("class", context),
                           StringAttr::get(context, classString));
  return DictionaryAttr::getWithSorted(context, {id});
}

/// Checks the annotations array for a matching annotation.
static bool hasAnnotation(ArrayAttr annotations, DictionaryAttr annotation) {
  return llvm::is_contained(annotations, annotation);
}

//===----------------------------------------------------------------------===//
// SharedParserConstants
//===----------------------------------------------------------------------===//

namespace {

/// A set of Target strings.
using TargetSet = StringSet<llvm::BumpPtrAllocator>;

/// This class refers to immutable values and annotations maintained globally by
/// the parser which can be referred to by any active parser, even those running
/// in parallel.  This is shared by all active parsers.
struct SharedParserConstants {
  SharedParserConstants(MLIRContext *context, FIRParserOptions options)
      : context(context), options(options),
        dontTouchAnnotation(getAnnotationOfClass(
            context, "firrtl.transforms.DontTouchAnnotation")),
        emptyArrayAttr(ArrayAttr::get(context, {})),
        loIdentifier(Identifier::get("lo", context)),
        hiIdentifier(Identifier::get("hi", context)),
        amountIdentifier(Identifier::get("amount", context)),
        fieldIndexIdentifier(Identifier::get("fieldIndex", context)),
        indexIdentifier(Identifier::get("index", context)) {}

  /// The context we're parsing into.
  MLIRContext *const context;

  // Options that control the behavior of the parser.
  const FIRParserOptions options;

  /// A mapping of targets to annotations.
  /// NOTE: Clients (other than the top level Circuit parser) should not mutate
  /// this.  Do not use `annotationMap[key]`, use `aM.lookup(key)` instead.
  llvm::StringMap<ArrayAttr> annotationMap;

  /// A set of all targets discovered in the circuit.  This is used after both
  /// Annotations and the FIRRTL circuit is parsed to check that nothing was
  /// unapplied from the annotationMap.  This, like the annotationMap, should
  /// not be mutated directly unless the client is the Circuit parser.
  TargetSet targetSet;

  /// Cached annotation for DontTouch.
  const DictionaryAttr dontTouchAnnotation;

  /// An empty array attribute.
  const ArrayAttr emptyArrayAttr;

  /// Cached identifiers used in primitives.
  const Identifier loIdentifier, hiIdentifier, amountIdentifier;
  const Identifier fieldIndexIdentifier, indexIdentifier;

private:
  SharedParserConstants(const SharedParserConstants &) = delete;
  void operator=(const SharedParserConstants &) = delete;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// FIRParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements logic common to all levels of the parser, including
/// things like types and helper logic.
struct FIRParser {
  FIRParser(SharedParserConstants &constants, FIRLexer &lexer)
      : constants(constants), lexer(lexer),
        locatorFilenameCache(constants.loIdentifier /*arbitrary non-null id*/) {
  }

  // Helper methods to get stuff from the shared parser constants.
  SharedParserConstants &getConstants() const { return constants; }
  MLIRContext *getContext() const { return constants.context; }

  FIRLexer &getLexer() { return lexer; }

  /// Return the indentation level of the specified token.
  Optional<unsigned> getIndentation() const {
    return lexer.getIndentation(getToken());
  }

  /// Return the current token the parser is inspecting.
  const FIRToken &getToken() const { return lexer.getToken(); }
  StringRef getTokenSpelling() const { return getToken().getSpelling(); }

  //===--------------------------------------------------------------------===//
  // Error Handling
  //===--------------------------------------------------------------------===//

  /// Emit an error and return failure.
  InFlightDiagnostic emitError(const Twine &message = {}) {
    return emitError(getToken().getLoc(), message);
  }
  InFlightDiagnostic emitError(SMLoc loc, const Twine &message = {});

  //===--------------------------------------------------------------------===//
  // Location Handling
  //===--------------------------------------------------------------------===//

  class LocWithInfo;

  /// Encode the specified source location information into an attribute for
  /// attachment to the IR.
  Location translateLocation(llvm::SMLoc loc) {
    return lexer.translateLocation(loc);
  }

  /// Parse an @info marker if present.  If so, fill in the specified Location,
  /// if not, ignore it.
  ParseResult parseOptionalInfoLocator(LocationAttr &result);

  /// Parse an optional name that may appear in Stop, Printf, or Verification
  /// statements.
  ParseResult parseOptionalName(StringAttr &name);

  //===--------------------------------------------------------------------===//
  // Annotation Parsing
  //===--------------------------------------------------------------------===//

  /// Parse a non-standard inline Annotation JSON blob if present.  This uses
  /// the info-like encoding of %[<JSON Blob>].
  ParseResult parseOptionalAnnotations(SMLoc &loc, StringRef &result);

  //===--------------------------------------------------------------------===//
  // Token Parsing
  //===--------------------------------------------------------------------===//

  /// If the current token has the specified kind, consume it and return true.
  /// If not, return false.
  bool consumeIf(FIRToken::Kind kind) {
    if (getToken().isNot(kind))
      return false;
    consumeToken(kind);
    return true;
  }

  /// Advance the current lexer onto the next token.
  ///
  /// This returns the consumed token.
  FIRToken consumeToken() {
    FIRToken consumedToken = getToken();
    assert(consumedToken.isNot(FIRToken::eof, FIRToken::error) &&
           "shouldn't advance past EOF or errors");
    lexer.lexToken();
    return consumedToken;
  }

  /// Advance the current lexer onto the next token, asserting what the expected
  /// current token is.  This is preferred to the above method because it leads
  /// to more self-documenting code with better checking.
  ///
  /// This returns the consumed token.
  FIRToken consumeToken(FIRToken::Kind kind) {
    FIRToken consumedToken = getToken();
    assert(consumedToken.is(kind) && "consumed an unexpected token");
    consumeToken();
    return consumedToken;
  }

  /// Capture the current token's spelling into the specified value.  This
  /// always succeeds.
  ParseResult parseGetSpelling(StringRef &spelling) {
    spelling = getTokenSpelling();
    return success();
  }

  /// Consume the specified token if present and return success.  On failure,
  /// output a diagnostic and return failure.
  ParseResult parseToken(FIRToken::Kind expectedToken, const Twine &message);

  /// Parse a list of elements, terminated with an arbitrary token.
  ParseResult parseListUntil(FIRToken::Kind rightToken,
                             const std::function<ParseResult()> &parseElement);

  //===--------------------------------------------------------------------===//
  // Common Parser Rules
  //===--------------------------------------------------------------------===//

  /// Parse 'intLit' into the specified value.
  ParseResult parseIntLit(APInt &result, const Twine &message);
  ParseResult parseIntLit(int64_t &result, const Twine &message);

  // Parse ('<' intLit '>')? setting result to -1 if not present.
  ParseResult parseOptionalWidth(int64_t &result);

  // Parse the 'id' grammar, which is an identifier or an allowed keyword.
  ParseResult parseId(StringRef &result, const Twine &message);
  ParseResult parseId(StringAttr &result, const Twine &message);
  ParseResult parseFieldId(StringRef &result, const Twine &message);
  ParseResult parseType(FIRRTLType &result, const Twine &message);

  ParseResult parseOptionalRUW(RUWAttr &result);

  //===--------------------------------------------------------------------===//
  // Annotation Utilities
  //===--------------------------------------------------------------------===//

  /// Convert the input "tokens" to a range of field IDs. Considering a FIRRTL
  /// aggregate type, "{foo: UInt<1>, bar: UInt<1>}[2]", tokens "[0].foo" will
  /// be converted to a field ID range of [2, 2]; tokens "[1]" will be converted
  /// to [4, 6]. The generated field ID range will then be attached to the
  /// firrtl::subAnnotationAttr in order to indicate the applicable fields of an
  /// annotation.
  Optional<unsigned> getFieldIDFromTokens(ArrayAttr tokens, SMLoc loc,
                                          Type type);

  /// In the input "annotations", an annotation will have a "target" entry when
  /// it is only applicable to part of what it is attached to. In this method,
  /// we convert the tokens contained by the "target" entry to a range of field
  /// IDs and return the converted annotations.
  ArrayAttr convertSubAnnotations(ArrayRef<Attribute> annotations, SMLoc loc,
                                  Type type);

  /// Split the input "annotations" into two parts: (1) Annotations applicable
  /// to all op results; (2) Annotations only applicable to one specific result.
  /// The second kind of annotations are store densely in an ArrayAttr and can
  /// be accessed using the result index.
  std::pair<ArrayAttr, ArrayAttr>
  splitAnnotations(ArrayRef<Attribute> annotations, SMLoc loc,
                   ArrayRef<std::pair<StringAttr, Type>> ports);

  /// Return the set of annotations for a given Target.
  ArrayAttr getAnnotations(ArrayRef<Twine> targets, SMLoc loc,
                           TargetSet &targetSet, Type type);

  /// Return the set of annotations for a given Target. If the operation has
  /// variadic results, such as MemOp and InstanceOp, this method should be used
  /// to get annotations.
  std::pair<ArrayAttr, ArrayAttr>
  getSplitAnnotations(ArrayRef<Twine> targets, SMLoc loc,
                      ArrayRef<std::pair<StringAttr, Type>> ports,
                      TargetSet &targetSet);

  /// Returns true if the annotation list contains the DontTouchAnnotation. This
  /// method is slightly more efficient than other lookup methods, because it
  /// uses a stashed copy of the annotation for lookup.
  bool hasDontTouch(ArrayAttr annotations) {
    return hasAnnotation(annotations, constants.dontTouchAnnotation);
  }

private:
  FIRParser(const FIRParser &) = delete;
  void operator=(const FIRParser &) = delete;

  /// FIRParser is subclassed and reinstantiated.  Do not add additional
  /// non-trivial state here, add it to SharedParserConstants.
  SharedParserConstants &constants;
  FIRLexer &lexer;

  /// This is a single-entry cache for filenames in locators.
  Identifier locatorFilenameCache;
  /// This is a single-entry cache for FileLineCol locations.
  FileLineColLoc fileLineColLocCache;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

InFlightDiagnostic FIRParser::emitError(SMLoc loc, const Twine &message) {
  auto diag = mlir::emitError(translateLocation(loc), message);

  // If we hit a parse error in response to a lexer error, then the lexer
  // already reported the error.
  if (getToken().is(FIRToken::error))
    diag.abandon();
  return diag;
}

//===----------------------------------------------------------------------===//
// Token Parsing
//===----------------------------------------------------------------------===//

/// Consume the specified token if present and return success.  On failure,
/// output a diagnostic and return failure.
ParseResult FIRParser::parseToken(FIRToken::Kind expectedToken,
                                  const Twine &message) {
  if (consumeIf(expectedToken))
    return success();
  return emitError(message);
}

/// Parse a list of elements, terminated with an arbitrary token.
ParseResult
FIRParser::parseListUntil(FIRToken::Kind rightToken,
                          const std::function<ParseResult()> &parseElement) {

  while (!consumeIf(rightToken)) {
    if (parseElement())
      return failure();
  }
  return success();
}

//===--------------------------------------------------------------------===//
// Location Processing
//===--------------------------------------------------------------------===//

/// This helper class is used to handle Info records, which specify higher level
/// symbolic source location, that may be missing from the file.  If the higher
/// level source information is missing, we fall back to the location in the
/// .fir file.
class FIRParser::LocWithInfo {
public:
  explicit LocWithInfo(SMLoc firLoc, FIRParser *parser)
      : parser(parser), firLoc(firLoc) {}

  SMLoc getFIRLoc() const { return firLoc; }

  Location getLoc() {
    if (infoLoc.hasValue())
      return infoLoc.getValue();
    auto result = parser->translateLocation(firLoc);
    infoLoc = result;
    return result;
  }

  /// Parse an @info marker if present and update our location.
  ParseResult parseOptionalInfo() {
    LocationAttr loc;
    if (failed(parser->parseOptionalInfoLocator(loc)))
      return failure();
    if (loc)
      infoLoc = loc;
    return success();
  }

  /// If we didn't parse an info locator for the specified value, this sets a
  /// default, overriding a fall back to a location in the .fir file.
  void setDefaultLoc(Location loc) {
    if (!infoLoc.hasValue())
      infoLoc = loc;
  }

private:
  FIRParser *const parser;

  /// This is the designated location in the .fir file for use when there is no
  /// @ info marker.
  SMLoc firLoc;

  /// This is the location specified by the @ marker if present.
  Optional<Location> infoLoc;
};

/// Parse an @info marker if present.  If so, fill in the specified Location,
/// if not, ignore it.
ParseResult FIRParser::parseOptionalInfoLocator(LocationAttr &result) {
  if (getToken().isNot(FIRToken::fileinfo))
    return success();

  auto loc = getToken().getLoc();

  // See if we can parse this token into a File/Line/Column record.  If not,
  // just ignore it with a warning.
  auto unknownFormat = [&]() -> ParseResult {
    mlir::emitWarning(translateLocation(loc),
                      "ignoring unknown @ info record format");
    return success();
  };

  auto spelling = getTokenSpelling();
  consumeToken(FIRToken::fileinfo);

  // The spelling of the token looks something like "@[Decoupled.scala 221:8]".
  if (!spelling.startswith("@[") || !spelling.endswith("]"))
    return unknownFormat();

  spelling = spelling.drop_front(2).drop_back(1);

  // Decode the locator in "spelling", returning the filename and filling in
  // lineNo and colNo on success.  On failure, this returns an empty filename.
  auto decodeLocator = [&](StringRef input, unsigned &resultLineNo,
                           unsigned &resultColNo) -> StringRef {
    // Split at the last space.
    auto spaceLoc = input.find_last_of(' ');
    if (spaceLoc == StringRef::npos)
      return {};

    auto filename = input.take_front(spaceLoc);
    auto lineAndColumn = input.drop_front(spaceLoc + 1);

    // Decode the line/column.  If the colon is missing, then it will be empty
    // here.
    StringRef lineStr, colStr;
    std::tie(lineStr, colStr) = lineAndColumn.split(':');

    // Decode the line number and the column number if present.
    if (lineStr.getAsInteger(10, resultLineNo))
      return {};
    if (!colStr.empty()) {
      if (colStr.front() != '{') {
        if (colStr.getAsInteger(10, resultColNo))
          return {};
      } else {
        // compound locator, just parse the first part for now
        if (colStr.drop_front().split(',').first.getAsInteger(10, resultColNo))
          return {};
      }
    }
    return filename;
  };

  // Decode the locator spelling, reporting an error if it is malformed.
  unsigned lineNo = 0, columnNo = 0;
  StringRef filename = decodeLocator(spelling, lineNo, columnNo);
  if (filename.empty())
    return unknownFormat();

  // If info locators are ignored, don't actually apply them.  We still do all
  // the verification above though.
  if (constants.options.ignoreInfoLocators)
    return success();

  /// Return an FileLineColLoc for the specified location, but use a bit of
  /// caching to reduce thrasing the MLIRContext.
  auto getFileLineColLoc = [&](StringRef filename, unsigned lineNo,
                               unsigned columnNo) -> FileLineColLoc {
    // Check our single-entry cache for this filename.
    Identifier filenameId = locatorFilenameCache;
    if (filenameId.str() != filename) {
      // We missed!  Get the right identifier.
      locatorFilenameCache = filenameId =
          Identifier::get(filename, getContext());

      // If we miss in the filename cache, we also miss in the FileLineColLoc
      // cache.
      return fileLineColLocCache =
                 FileLineColLoc::get(filenameId, lineNo, columnNo);
    }

    // If we hit the filename cache, check the FileLineColLoc cache.
    auto result = fileLineColLocCache;
    if (result && result.getLine() == lineNo && result.getColumn() == columnNo)
      return result;

    return fileLineColLocCache =
               FileLineColLoc::get(filenameId, lineNo, columnNo);
  };

  // Compound locators will be combined with spaces, like:
  //  @[Foo.scala 123:4 Bar.scala 309:14]
  // and at this point will be parsed as a-long-string-with-two-spaces at
  // 309:14.   We'd like to parse this into two things and represent it as an
  // MLIR fused locator, but we want to be conservatively safe for filenames
  // that have a space in it.  As such, we are careful to make sure we can
  // decode the filename/loc of the result.  If so, we accumulate results,
  // backward, in this vector.
  SmallVector<Location> extraLocs;
  auto spaceLoc = filename.find_last_of(' ');
  while (spaceLoc != StringRef::npos) {
    // Try decoding the thing before the space.  Validates that there is another
    // space and that the file/line can be decoded in that substring.
    unsigned nextLineNo = 0, nextColumnNo = 0;
    auto nextFilename =
        decodeLocator(filename.take_front(spaceLoc), nextLineNo, nextColumnNo);

    // On failure we didn't have a joined locator.
    if (nextFilename.empty())
      break;

    // On success, remember what we already parsed (Bar.Scala / 309:14), and
    // move on to the next chunk.
    auto loc =
        getFileLineColLoc(filename.drop_front(spaceLoc + 1), lineNo, columnNo);
    extraLocs.push_back(loc);
    filename = nextFilename;
    lineNo = nextLineNo;
    columnNo = nextColumnNo;
    spaceLoc = filename.find_last_of(' ');
  }

  result = getFileLineColLoc(filename, lineNo, columnNo);
  if (!extraLocs.empty()) {
    extraLocs.push_back(result);
    std::reverse(extraLocs.begin(), extraLocs.end());
    result = FusedLoc::get(getContext(), extraLocs);
  }
  return success();
}

/// Parse an optional trailing name that may show up on assert, assume, cover,
/// stop, or printf.
///
/// optional_name ::= ( ':' id )?
ParseResult FIRParser::parseOptionalName(StringAttr &name) {

  if (getToken().isNot(FIRToken::colon)) {
    name = StringAttr::get(getContext(), "");
    return success();
  }

  consumeToken(FIRToken::colon);
  StringRef nameRef;
  if (parseId(nameRef, "expected result name"))
    return failure();

  name = StringAttr::get(getContext(), nameRef);

  return success();
}

//===--------------------------------------------------------------------===//
// Annotation Handling
//===--------------------------------------------------------------------===//

/// Parse a non-standard inline Annotation JSON blob if present.  This uses
/// the info-like encoding of %[<JSON Blob>].
ParseResult FIRParser::parseOptionalAnnotations(SMLoc &loc, StringRef &result) {

  if (getToken().isNot(FIRToken::inlineannotation))
    return success();

  loc = getToken().getLoc();

  result = getTokenSpelling().drop_front(2).drop_back(1);
  consumeToken(FIRToken::inlineannotation);

  return success();
}

//===--------------------------------------------------------------------===//
// Common Parser Rules
//===--------------------------------------------------------------------===//

/// intLit    ::= UnsignedInt
///           ::= SignedInt
///           ::= HexLit
///           ::= OctalLit
///           ::= BinaryLit
/// HexLit    ::= '"' 'h' ( '+' | '-' )? ( HexDigit )+ '"'
/// OctalLit  ::= '"' 'o' ( '+' | '-' )? ( OctalDigit )+ '"'
/// BinaryLit ::= '"' 'b' ( '+' | '-' )? ( BinaryDigit )+ '"'
///
ParseResult FIRParser::parseIntLit(APInt &result, const Twine &message) {
  auto spelling = getTokenSpelling();
  switch (getToken().getKind()) {
  case FIRToken::integer:
    if (spelling.getAsInteger(10, result))
      return emitError(message), failure();

    // Make sure that the returned APInt has a zero at the top so clients don't
    // confuse it with a negative number.
    if (result.isNegative())
      result = result.zext(result.getBitWidth() + 1);

    consumeToken(FIRToken::integer);
    return success();

  case FIRToken::signed_integer:
    assert(spelling[0] == '+' || spelling[0] == '-');
    if (spelling.drop_front().getAsInteger(10, result))
      return emitError(message), failure();

    // Make sure that the returned APInt has a zero at the top so clients don't
    // confuse it with a negative number.
    if (result.isNegative())
      result = result.zext(result.getBitWidth() + 1);

    if (spelling[0] == '-')
      result = -result;
    consumeToken(FIRToken::signed_integer);
    return success();
  case FIRToken::string: {
    // Drop the quotes.
    assert(spelling.front() == '"' && spelling.back() == '"');
    spelling = spelling.drop_back().drop_front();

    // Decode the base.
    unsigned base;
    switch (spelling.empty() ? ' ' : spelling.front()) {
    case 'h':
      base = 16;
      break;
    case 'o':
      base = 8;
      break;
    case 'b':
      base = 2;
      break;
    default:
      return emitError("expected base specifier (h/o/b) in integer literal"),
             failure();
    }
    spelling = spelling.drop_front();

    // Handle the optional sign.
    bool isNegative = false;
    if (!spelling.empty() && spelling.front() == '+')
      spelling = spelling.drop_front();
    else if (!spelling.empty() && spelling.front() == '-') {
      isNegative = true;
      spelling = spelling.drop_front();
    }

    // Parse the digits.
    if (spelling.empty())
      return emitError("expected digits in integer literal"), failure();

    if (spelling.getAsInteger(base, result))
      return emitError("invalid character in integer literal"), failure();

    // We just parsed the positive version of this number.  Make sure it has
    // a zero at the top so clients don't confuse it with a negative number and
    // so the negation (in the case of a negative sign) doesn't overflow.
    if (result.isNegative())
      result = result.zext(result.getBitWidth() + 1);

    if (isNegative)
      result = -result;

    consumeToken(FIRToken::string);
    return success();
  }

  default:
    return emitError("expected integer literal"), failure();
  }
}

ParseResult FIRParser::parseIntLit(int64_t &result, const Twine &message) {
  APInt value;
  auto loc = getToken().getLoc();
  if (parseIntLit(value, message))
    return failure();

  result = (int64_t)value.getLimitedValue();
  if (result != value)
    return emitError(loc, "value is too big to handle"), failure();
  return success();
}

// optional-width ::= ('<' intLit '>')?
//
// This returns with result equal to -1 if not present.
ParseResult FIRParser::parseOptionalWidth(int64_t &result) {
  if (!consumeIf(FIRToken::less))
    return result = -1, success();

  // Parse a width specifier if present.
  auto widthLoc = getToken().getLoc();
  if (parseIntLit(result, "expected width") ||
      parseToken(FIRToken::greater, "expected >"))
    return failure();

  if (result < 0)
    return emitError(widthLoc, "invalid width specifier"), failure();

  return success();
}

/// id  ::= Id | keywordAsId
///
/// Parse the 'id' grammar, which is an identifier or an allowed keyword.  On
/// success, this returns the identifier in the result attribute.
ParseResult FIRParser::parseId(StringRef &result, const Twine &message) {
  switch (getToken().getKind()) {
  // The most common case is an identifier.
  case FIRToken::identifier:
// Otherwise it may be a keyword that we're allowing in an id position.
#define TOK_KEYWORD(spelling) case FIRToken::kw_##spelling:
#include "FIRTokenKinds.def"

    // Yep, this is a valid 'id'.  Turn it into an attribute.
    result = getTokenSpelling();
    consumeToken();
    return success();

  default:
    emitError(message);
    return failure();
  }
}

ParseResult FIRParser::parseId(StringAttr &result, const Twine &message) {
  StringRef name;
  if (parseId(name, message))
    return failure();

  result = StringAttr::get(getContext(), name);
  return success();
}

/// fieldId ::= Id
///         ::= RelaxedId
///         ::= UnsignedInt
///         ::= keywordAsId
///
ParseResult FIRParser::parseFieldId(StringRef &result, const Twine &message) {
  // Handle the UnsignedInt case.
  result = getTokenSpelling();
  if (consumeIf(FIRToken::integer))
    return success();

  // FIXME: Handle RelaxedId

  // Otherwise, it must be Id or keywordAsId.
  if (parseId(result, message))
    return failure();

  return success();
}

/// type ::= 'Clock'
///      ::= 'Reset'
///      ::= 'AsyncReset'
///      ::= 'UInt' optional-width
///      ::= 'SInt' optional-width
///      ::= 'Analog' optional-width
///      ::= {' field* '}'
///      ::= type '[' intLit ']'
///
/// field: 'flip'? fieldId ':' type
///
ParseResult FIRParser::parseType(FIRRTLType &result, const Twine &message) {
  switch (getToken().getKind()) {
  default:
    return emitError(message), failure();

  case FIRToken::kw_Clock:
    consumeToken(FIRToken::kw_Clock);
    result = ClockType::get(getContext());
    break;

  case FIRToken::kw_Reset:
    consumeToken(FIRToken::kw_Reset);
    result = ResetType::get(getContext());
    break;

  case FIRToken::kw_AsyncReset:
    consumeToken(FIRToken::kw_AsyncReset);
    result = AsyncResetType::get(getContext());
    break;

  case FIRToken::kw_UInt:
  case FIRToken::kw_SInt:
  case FIRToken::kw_Analog: {
    auto kind = getToken().getKind();
    consumeToken();

    // Parse a width specifier if present.
    int64_t width;
    if (parseOptionalWidth(width))
      return failure();

    if (kind == FIRToken::kw_SInt)
      result = SIntType::get(getContext(), width);
    else if (kind == FIRToken::kw_UInt)
      result = UIntType::get(getContext(), width);
    else {
      assert(kind == FIRToken::kw_Analog);
      result = AnalogType::get(getContext(), width);
    }
    break;
  }

  case FIRToken::l_brace: {
    consumeToken(FIRToken::l_brace);

    SmallVector<BundleType::BundleElement, 4> elements;
    if (parseListUntil(FIRToken::r_brace, [&]() -> ParseResult {
          bool isFlipped = consumeIf(FIRToken::kw_flip);

          StringRef fieldName;
          FIRRTLType type;
          if (parseFieldId(fieldName, "expected bundle field name") ||
              parseToken(FIRToken::colon, "expected ':' in bundle") ||
              parseType(type, "expected bundle field type"))
            return failure();

          elements.push_back(
              {StringAttr::get(getContext(), fieldName), isFlipped, type});
          return success();
        }))
      return failure();
    result = BundleType::get(elements, getContext());
    break;
  }
  }

  // Handle postfix vector sizes.
  while (consumeIf(FIRToken::l_square)) {
    auto sizeLoc = getToken().getLoc();
    int64_t size;
    if (parseIntLit(size, "expected width") ||
        parseToken(FIRToken::r_square, "expected ]"))
      return failure();

    if (size < 0)
      return emitError(sizeLoc, "invalid size specifier"), failure();

    result = FVectorType::get(result, size);
  }

  return success();
}

/// ruw ::= 'old' | 'new' | 'undefined'
ParseResult FIRParser::parseOptionalRUW(RUWAttr &result) {
  switch (getToken().getKind()) {
  default:
    break;

  case FIRToken::kw_old:
    result = RUWAttr::Old;
    consumeToken(FIRToken::kw_old);
    break;
  case FIRToken::kw_new:
    result = RUWAttr::New;
    consumeToken(FIRToken::kw_new);
    break;
  case FIRToken::kw_undefined:
    result = RUWAttr::Undefined;
    consumeToken(FIRToken::kw_undefined);
    break;
  }

  return success();
}

/// Convert the input "tokens" to a range of field IDs. Considering a FIRRTL
/// aggregate type, "{foo: UInt<1>, bar: UInt<1>}[2]", tokens "[0].foo" will be
/// converted to a field ID range of [2, 2]; tokens "[1]" will be converted to
/// [4, 6]. The generated field ID range will then be attached to the
/// firrtl::subAnnotationAttr in order to indicate the applicable fields of an
/// annotation.
Optional<unsigned> FIRParser::getFieldIDFromTokens(ArrayAttr tokens, SMLoc loc,
                                                   Type type) {
  if (!type)
    return None;
  if (tokens.empty())
    return 0;

  auto currentType = type.cast<FIRRTLType>();
  unsigned id = 0;

  auto getMessage = [&](unsigned tokenIdx) {
    // Construct a string for error emission.
    SmallString<16> message("the " + std::to_string(tokenIdx) +
                            "-th token of ");
    for (auto token : tokens) {
      if (auto intAttr = token.dyn_cast<IntegerAttr>()) {
        message.push_back('[');
        intAttr.getValue().toStringSigned(message);
        message.push_back(']');
      } else if (auto strAttr = token.dyn_cast<StringAttr>()) {
        message.push_back('.');
        message.append(strAttr.getValue());
      }
    }
    return message;
  };

  unsigned tokenIdx = 0;
  for (auto token : tokens) {
    ++tokenIdx;
    if (auto bundleType = currentType.dyn_cast<BundleType>()) {
      auto subFieldAttr = token.dyn_cast<StringAttr>();
      if (!subFieldAttr) {
        emitError(loc, "expect a string for " + getMessage(tokenIdx));
        return None;
      }

      // Token should point to valid sub-field.
      auto subField = subFieldAttr.getValue();
      auto index = bundleType.getElementIndex(subField);
      if (!index) {
        emitError(loc, getMessage(tokenIdx) + " is not found in the bundle");
        return None;
      }

      id += bundleType.getFieldID(index.getValue());
      currentType = bundleType.getElementType(subField);
      continue;
    }

    if (auto vectorType = currentType.dyn_cast<FVectorType>()) {
      auto subIndexAttr = token.dyn_cast<IntegerAttr>();
      if (!subIndexAttr) {
        emitError(loc, "expect an integer for " + getMessage(tokenIdx));
        return None;
      }

      // Token should be a valid index of the vector.
      auto subIndex = subIndexAttr.getValue().getSExtValue();
      if (subIndex < 0 || subIndex >= vectorType.getNumElements()) {
        emitError(loc, getMessage(tokenIdx) + " is out of range in the vector");
        return None;
      }

      id += vectorType.getFieldID(subIndex);
      currentType = vectorType.getElementType();
      continue;
    }

    emitError(loc, getMessage(tokenIdx) + " expects an aggregate type");
    return None;
  }

  return id;
}

/// In the input "annotations", an annotation will have a "target" entry when it
/// is only applicable to part of what it is attached to. In this method, we
/// convert the tokens contained by the "target" entry to a range of field IDs
/// and return the converted annotations.
ArrayAttr FIRParser::convertSubAnnotations(ArrayRef<Attribute> annotations,
                                           SMLoc loc, Type type = nullptr) {
  // This stores the annotations applicable to all fields of the op result.
  SmallVector<Attribute, 8> annotationVec;

  for (auto a : annotations) {
    // If the annotation does not contain a "target" entry, it is applicable to
    // all fields of the op result.
    auto dict = a.cast<DictionaryAttr>();
    auto targetAttr = dict.get("target");
    if (!targetAttr) {
      annotationVec.push_back(a);
      continue;
    }

    // Otherwise, the annotation is only applicable to part of the op result.
    // Get a range of field IDs to indicate the applicable fields of the
    // annotation.
    auto fieldID =
        getFieldIDFromTokens(targetAttr.cast<ArrayAttr>(), loc, type);
    if (!fieldID)
      continue;

    // Remove the "target" entry from the annotation.
    NamedAttrList modAttr;
    for (auto attr : dict.getValue()) {
      // Ignore the actual target annotation, but copy the rest of annotations.
      if (attr.first.str() == "target")
        continue;
      modAttr.push_back(attr);
    }

    // Construct the SubAnnotationAttr for the annotation.
    auto subAnnotation =
        SubAnnotationAttr::get(constants.context, fieldID.getValue(),
                               DictionaryAttr::get(constants.context, modAttr));

    annotationVec.push_back(subAnnotation);
  }

  return ArrayAttr::get(constants.context, annotationVec);
}

/// Split the input "annotations" into two parts: (1) Annotations applicable to
/// all op results; (2) Annotations only applicable to one specific result. The
/// second kind of annotations are store densely in an ArrayAttr and can be
/// accessed using the result index.
std::pair<ArrayAttr, ArrayAttr>
FIRParser::splitAnnotations(ArrayRef<Attribute> annotations, SMLoc loc,
                            ArrayRef<std::pair<StringAttr, Type>> ports) {
  // This stores the annotations applicable to all op ports.
  SmallVector<Attribute, 8> annotationVec;
  // This stores the mapping between port names and port-specific annotations.
  llvm::StringMap<SmallVector<Attribute, 4>> portAnnotationMap;

  for (auto a : annotations) {
    // If the annotation does not contain a "target" entry, it is applicable to
    // all op ports.
    auto dict = a.cast<DictionaryAttr>();
    auto targetAttr = dict.get("target");
    if (!targetAttr) {
      annotationVec.push_back(a);
      continue;
    }

    // The first token should always indicate the port name.
    auto targetTokens = targetAttr.cast<ArrayAttr>().getValue();
    auto portName = targetTokens[0].dyn_cast<StringAttr>();
    if (!portName) {
      emitError(loc, "the first token is invalid");
      continue;
    }

    // If there's more than one tokens, the remaining tokens should be added
    // into the annotation as a "target" field.
    NamedAttrList modAttr;
    for (auto attr : dict.getValue()) {
      if (attr.first.str() == "target") {
        if (targetTokens.size() > 1) {
          auto newTargetAttr =
              ArrayAttr::get(constants.context, targetTokens.drop_front());
          modAttr.push_back(NamedAttribute(attr.first, newTargetAttr));
        }
        continue;
      }
      modAttr.push_back(attr);
    }

    portAnnotationMap[portName.getValue()].push_back(
        DictionaryAttr::get(constants.context, modAttr));
  }

  SmallVector<Attribute, 16> portAnnotationVec;
  for (auto port : ports) {
    auto portAnnotation = portAnnotationMap.lookup(port.first.getValue());
    portAnnotationVec.push_back(
        convertSubAnnotations(portAnnotation, loc, port.second));
  }

  return {ArrayAttr::get(constants.context, annotationVec),
          ArrayAttr::get(constants.context, portAnnotationVec)};
}

/// Return the set of annotations for a given Target.
ArrayAttr FIRParser::getAnnotations(ArrayRef<Twine> targets, SMLoc loc,
                                    TargetSet &targetSet, Type type = nullptr) {
  // Early exit if no annotations exist.  This avoids the cost of constructing
  // strings representing targets if no annotation can possibly exist.
  if (constants.annotationMap.empty())
    return constants.emptyArrayAttr;

  // Stack the annotations for all targets.
  SmallVector<Attribute, 4> annotations;
  for (auto target : targets) {
    // Flatten the input twine into a SmallVector for lookup.
    SmallString<64> targetStr;
    target.toVector(targetStr);

    // Record that we've seen this target.
    targetSet.insert(targetStr);

    // Note: We are not allowed to mutate annotationMap here.  Make sure to only
    // use non-mutating methods like `lookup`, not mutating ones like `am[key]`.
    if (auto result = constants.annotationMap.lookup(targetStr)) {
      if (!result.empty())
        annotations.append(result.begin(), result.end());
    }
  }

  if (!annotations.empty())
    return convertSubAnnotations(annotations, loc, type);
  return constants.emptyArrayAttr;
}

/// Return the set of annotations for a given Target. If the operation has
/// variadic results, such as MemOp and InstanceOp, this method should be used
/// to get annotations.
std::pair<ArrayAttr, ArrayAttr>
FIRParser::getSplitAnnotations(ArrayRef<Twine> targets, SMLoc loc,
                               ArrayRef<std::pair<StringAttr, Type>> ports,
                               TargetSet &targetSet) {
  // Early exit if no annotations exist.  This avoids the cost of constructing
  // strings representing targets if no annotation can possibly exist.
  if (constants.annotationMap.empty()) {
    SmallVector<Attribute, 4> portAnnotations(
        ports.size(), ArrayAttr::get(constants.context, {}));
    return {constants.emptyArrayAttr,
            ArrayAttr::get(constants.context, portAnnotations)};
  }

  // Stack the annotations for all targets.
  SmallVector<Attribute, 4> annotations;
  for (auto target : targets) {
    // Flatten the input twine into a SmallVector for lookup.
    SmallString<64> targetStr;
    target.toVector(targetStr);

    // Record that we've seen this target.
    targetSet.insert(targetStr);

    // Note: We are not allowed to mutate annotationMap here.  Make sure to only
    // use non-mutating methods like `lookup`, not mutating ones like `am[key]`.
    if (auto result = constants.annotationMap.lookup(targetStr)) {
      if (!result.empty())
        annotations.append(result.begin(), result.end());
    }
  }

  if (!annotations.empty())
    return splitAnnotations(annotations, loc, ports);

  SmallVector<Attribute, 4> portAnnotations(
      ports.size(), ArrayAttr::get(constants.context, {}));
  return {constants.emptyArrayAttr,
          ArrayAttr::get(constants.context, portAnnotations)};
};

//===----------------------------------------------------------------------===//
// FIRModuleContext
//===----------------------------------------------------------------------===//

// Entries in a symbol table are either an mlir::Value for the operation that
// defines the value or an unbundled ID tracking the index in the
// UnbundledValues list.
using UnbundledID = llvm::PointerEmbeddedInt<unsigned, 31>;
using SymbolValueEntry = llvm::PointerUnion<Value, UnbundledID>;

using ModuleSymbolTable =
    llvm::StringMap<std::pair<SMLoc, SymbolValueEntry>, llvm::BumpPtrAllocator>;
using ModuleSymbolTableEntry =
    llvm::StringMapEntry<std::pair<SMLoc, SymbolValueEntry>>;

using UnbundledValueEntry = SmallVector<std::pair<Attribute, Value>>;
using UnbundledValuesList = std::vector<UnbundledValueEntry>;

namespace {
/// This struct provides context information that is global to the module we're
/// currently parsing into.
struct FIRModuleContext : public FIRParser {
  explicit FIRModuleContext(SharedParserConstants &constants, FIRLexer &lexer,
                            std::string moduleTarget)
      : FIRParser(constants, lexer), moduleTarget(std::move(moduleTarget)) {}

  /// This is the module target used by annotations referring to this module.
  std::string moduleTarget;

  // The expression-oriented nature of firrtl syntax produces tons of constant
  // nodes which are obviously redundant.  Instead of literally producing them
  // in the parser, do an implicit CSE to reduce parse time and silliness in the
  // resulting IR.
  llvm::DenseMap<std::pair<Attribute, Type>, Value> constantCache;

  /// Add a symbol entry with the specified name, returning failure if the name
  /// is already defined.
  ParseResult addSymbolEntry(StringRef name, SymbolValueEntry entry, SMLoc loc,
                             bool insertNameIntoGlobalScope = false);
  ParseResult addSymbolEntry(StringRef name, Value value, SMLoc loc,
                             bool insertNameIntoGlobalScope = false) {
    return addSymbolEntry(name, SymbolValueEntry(value), loc,
                          insertNameIntoGlobalScope);
  }

  /// Resolved a symbol table entry to a value.  Emission of error is optional.
  ParseResult resolveSymbolEntry(Value &result, SymbolValueEntry &entry,
                                 SMLoc loc, bool fatal = true);

  /// Resolved a symbol table entry if it is an expanded bundle e.g. from an
  /// instance.  Emission of error is optional.
  ParseResult resolveSymbolEntry(Value &result, SymbolValueEntry &entry,
                                 StringRef field, SMLoc loc);

  /// Look up the specified name, emitting an error and returning failure if the
  /// name is unknown.
  ParseResult lookupSymbolEntry(SymbolValueEntry &result, StringRef name,
                                SMLoc loc);

  UnbundledValueEntry &getUnbundledEntry(unsigned index) {
    assert(index < unbundledValues.size());
    return unbundledValues[index];
  }

  /// This contains one entry for each value in FIRRTL that is represented as a
  /// bundle type in the FIRRTL spec but for which we represent as an exploded
  /// set of elements in the FIRRTL dialect.
  UnbundledValuesList unbundledValues;

  /// Provide a symbol table scope that automatically pops all the entries off
  /// the symbol table when the scope is exited.
  struct SymbolTableScope {
    SymbolTableScope(FIRModuleContext &moduleContext)
        : moduleContext(moduleContext),
          prevScopedDecls(moduleContext.currentScopeCollector) {
      moduleContext.currentScopeCollector = &scopedDecls;
    }
    ~SymbolTableScope() {
      // Mark all entries in this scope as being invalid.  We track validity
      // through the SMLoc field instead of deleting entries.
      for (auto *entryPtr : scopedDecls)
        entryPtr->second.first = SMLoc();
      moduleContext.currentScopeCollector = prevScopedDecls;
    }

  private:
    void operator=(const SymbolTableScope &) = delete;
    SymbolTableScope(const SymbolTableScope &) = delete;

    FIRModuleContext &moduleContext;
    std::vector<ModuleSymbolTableEntry *> *prevScopedDecls;
    std::vector<ModuleSymbolTableEntry *> scopedDecls;
  };

  /// A set of all Annotation Targets found in this module.  This is used to
  /// facilitate checking if any Annotations exist which do not match to
  /// Targets.
  TargetSet targetsInModule;

private:
  /// This symbol table holds the names of ports, wires, and other local decls.
  /// This is scoped because conditional statements introduce subscopes.
  ModuleSymbolTable symbolTable;

  /// If non-null, all new entries added to the symbol table are added to this
  /// list.  This allows us to "pop" the entries by resetting them to null when
  /// scope is exited.
  std::vector<ModuleSymbolTableEntry *> *currentScopeCollector = nullptr;
};

} // end anonymous namespace

/// Add a symbol entry with the specified name, returning failure if the name
/// is already defined.
///
/// When 'insertNameIntoGlobalScope' is true, we don't allow the name to be
/// popped.  This is a workaround for (firrtl scala bug) that should eventually
/// be fixed.
ParseResult FIRModuleContext::addSymbolEntry(StringRef name,
                                             SymbolValueEntry entry, SMLoc loc,
                                             bool insertNameIntoGlobalScope) {
  // Do a lookup by trying to do an insertion.  Do so in a way that we can tell
  // if we hit a missing element (SMLoc is null).
  auto entryIt =
      symbolTable.try_emplace(name, SMLoc(), SymbolValueEntry()).first;
  if (entryIt->second.first.isValid()) {
    emitError(loc, "redefinition of name '" + name + "'")
            .attachNote(translateLocation(entryIt->second.first))
        << "previous definition here";
    return failure();
  }

  // If we didn't have a hit, then record the location, and remember that this
  // was new to this scope.
  entryIt->second = {loc, entry};
  if (currentScopeCollector && !insertNameIntoGlobalScope)
    currentScopeCollector->push_back(&*entryIt);

  return success();
}

/// Look up the specified name, emitting an error and returning null if the
/// name is unknown.
ParseResult FIRModuleContext::lookupSymbolEntry(SymbolValueEntry &result,
                                                StringRef name, SMLoc loc) {
  auto &entry = symbolTable[name];
  if (!entry.first.isValid())
    return emitError(loc, "use of unknown declaration '" + name + "'");
  result = entry.second;
  assert(result && "name in symbol table without definition");
  return success();
}

ParseResult FIRModuleContext::resolveSymbolEntry(Value &result,
                                                 SymbolValueEntry &entry,
                                                 SMLoc loc, bool fatal) {
  if (!entry.is<Value>()) {
    if (fatal)
      emitError(loc, "bundle value should only be used from subfield");
    return failure();
  }
  result = entry.get<Value>();
  return success();
}

ParseResult FIRModuleContext::resolveSymbolEntry(Value &result,
                                                 SymbolValueEntry &entry,
                                                 StringRef fieldName,
                                                 SMLoc loc) {
  if (!entry.is<UnbundledID>()) {
    emitError(loc, "value should not be used from subfield");
    return failure();
  }

  auto fieldAttr = StringAttr::get(getContext(), fieldName);

  unsigned unbundledId = entry.get<UnbundledID>() - 1;
  assert(unbundledId < unbundledValues.size());
  UnbundledValueEntry &ubEntry = unbundledValues[unbundledId];
  for (auto elt : ubEntry) {
    if (elt.first == fieldAttr) {
      result = elt.second;
      break;
    }
  }
  if (!result) {
    emitError(loc, "use of invalid field name '")
        << fieldName << "' on bundle value";
    return failure();
  }

  return success();
}

//===----------------------------------------------------------------------===//
// FIRStmtParser
//===----------------------------------------------------------------------===//

namespace {
/// This class is used when building expression nodes for a statement: we need
/// to parse a bunch of expressions and build MLIR operations for them, and then
/// we see the locator that specifies the location for those operations
/// afterward.
///
/// It is wasteful in time and memory to create a bunch of temporary FileLineCol
/// location's that point into the .fir file when they're destined to get
/// overwritten by a location specified by a Locator.  To avoid this, we create
/// all of the operations with a temporary location on them, then remember the
/// [Operation*, SMLoc] pair for the newly created operation.
///
/// At the end of the operation we'll see a Locator (or not).  If we see a
/// locator, we apply it to all the operations we've parsed and we're done.  If
/// not, we lazily create the locators in the .fir file.
struct LazyLocationListener : public OpBuilder::Listener {
  LazyLocationListener(OpBuilder &builder) : builder(builder) {
    assert(builder.getListener() == nullptr);
    builder.setListener(this);
  }

  ~LazyLocationListener() {
    assert(subOps.empty() && "didn't process parsed operations");
    assert(builder.getListener() == this);
    builder.setListener(nullptr);
  }

  void startStatement() {
    assert(!isActive && "Already processing a statement");
    isActive = true;
  }

  /// This is called when done with each statement.  This applies the locations
  /// to each statement.
  void endStatement(FIRParser &parser) {
    assert(isActive && "Not parsing a statement");

    // If we have a symbolic location, apply it to any subOps specified.
    if (infoLoc) {
      for (auto opAndSMLoc : subOps)
        opAndSMLoc.first->setLoc(infoLoc);
    } else {
      // If we don't, translate all the individual SMLoc's to Location objects
      // in the .fir file.
      for (auto opAndSMLoc : subOps)
        opAndSMLoc.first->setLoc(parser.translateLocation(opAndSMLoc.second));
    }

    // Reset our state.
    isActive = false;
    infoLoc = LocationAttr();
    currentSMLoc = SMLoc();
    subOps.clear();
  }

  /// Specify the location to be used for the next operations that are created.
  void setLoc(SMLoc loc) { currentSMLoc = loc; }

  /// When a @Info locator is parsed, this method captures it.
  void setInfoLoc(LocationAttr loc) {
    assert(!infoLoc && "Info location multiply specified");
    infoLoc = loc;
  }

  // Notification handler for when an operation is inserted into the builder.
  /// `op` is the operation that was inserted.
  void notifyOperationInserted(Operation *op) override {
    assert(currentSMLoc != SMLoc() && "No .fir file location specified");
    assert(isActive && "Not parsing a statement");
    subOps.push_back({op, currentSMLoc});
  }

private:
  /// This is set to true while parsing a statement.  It is used for assertions.
  bool isActive = false;

  /// This is the current position in the source file that the next operation
  /// will be parsed into.
  SMLoc currentSMLoc;

  /// This is the @ location attribute for the current statement, or null if
  /// not set.
  LocationAttr infoLoc;

  /// This is the builder we're installed into.
  OpBuilder &builder;

  /// This is the set of operations we've enqueued along with their location in
  /// the source file.
  SmallVector<std::pair<Operation *, SMLoc>, 8> subOps;

  void operator=(const LazyLocationListener &) = delete;
  LazyLocationListener(const LazyLocationListener &) = delete;
};
} // end anonymous namespace

namespace {
/// This class implements logic and state for parsing statements, suites, and
/// similar module body constructs.
struct FIRStmtParser : public FIRParser {
  explicit FIRStmtParser(Block &blockToInsertInto,
                         FIRModuleContext &moduleContext)
      : FIRParser(moduleContext.getConstants(), moduleContext.getLexer()),
        builder(mlir::ImplicitLocOpBuilder::atBlockEnd(
            UnknownLoc::get(getContext()), &blockToInsertInto)),
        locationProcessor(this->builder), moduleContext(moduleContext) {}

  ParseResult parseSimpleStmt(unsigned stmtIndent);
  ParseResult parseSimpleStmtBlock(unsigned indent);

  /// Return the current modulet target, e.g., "~Foo|Bar".
  StringRef getModuleTarget() { return moduleContext.moduleTarget; }

  ParseResult parseSimpleStmtImpl(unsigned stmtIndent);

  /// Attach invalid values to every element of the value.
  void emitInvalidate(Value val, Flow flow);

  // The FIRRTL specification describes Invalidates as a statement with
  // implicit connect semantics.  The FIRRTL dialect models it as a primitive
  // that returns an "Invalid Value", followed by an explicit connect to make
  // the representation simpler and more consistent.
  void emitInvalidate(Value val) { emitInvalidate(val, foldFlow(val)); }

  /// Parse an @info marker if present and inform locationProcessor about it.
  ParseResult parseOptionalInfo() {
    LocationAttr loc;
    if (failed(parseOptionalInfoLocator(loc)))
      return failure();
    locationProcessor.setInfoLoc(loc);
    return success();
  }

  // Exp Parsing
  ParseResult parseExpImpl(Value &result, const Twine &message,
                           bool isLeadingStmt);
  ParseResult parseExp(Value &result, const Twine &message) {
    return parseExpImpl(result, message, /*isLeadingStmt:*/ false);
  }
  ParseResult parseExpLeadingStmt(Value &result, const Twine &message) {
    return parseExpImpl(result, message, /*isLeadingStmt:*/ true);
  }

  ParseResult parseOptionalExpPostscript(Value &result);
  ParseResult parsePostFixFieldId(Value &result);
  ParseResult parsePostFixIntSubscript(Value &result);
  ParseResult parsePostFixDynamicSubscript(Value &result);
  ParseResult parsePrimExp(Value &result);
  ParseResult parseIntegerLiteralExp(Value &result);

  Optional<ParseResult> parseExpWithLeadingKeyword(FIRToken keyword);

  // Stmt Parsing
  ParseResult parseAttach();
  ParseResult parseMemPort(MemDirAttr direction);
  ParseResult parsePrintf();
  ParseResult parseSkip();
  ParseResult parseStop();
  ParseResult parseAssert();
  ParseResult parseAssume();
  ParseResult parseCover();
  ParseResult parseWhen(unsigned whenIndent);
  ParseResult parseLeadingExpStmt(Value lhs);

  // Declarations
  ParseResult parseInstance();
  ParseResult parseCMem();
  ParseResult parseSMem();
  ParseResult parseMem(unsigned memIndent);
  ParseResult parseNode();
  ParseResult parseWire();
  ParseResult parseRegister(unsigned regIndent);

  // The builder to build into.
  ImplicitLocOpBuilder builder;
  LazyLocationListener locationProcessor;

  // Extra information maintained across a module.
  FIRModuleContext &moduleContext;
};

} // end anonymous namespace

/// Attach invalid values to every element of the value.
void FIRStmtParser::emitInvalidate(Value val, Flow flow) {
  auto tpe = val.getType().cast<FIRRTLType>();

  // Recurse until we hit leaves.  Connect any leaves which have sink or
  // duplex flow.
  //
  // TODO: This is very similar to connect expansion in the LowerTypes pass
  // works.  Find a way to unify this with methods common to LowerTypes or to
  // have LowerTypes to the actual work here, e.g., emitting a partial connect
  // to only the leaf sources.
  TypeSwitch<FIRRTLType>(tpe)
      .Case<BundleType>([&](auto tpe) {
        for (size_t i = 0, e = tpe.getNumElements(); i < e; ++i) {
          auto subfield = builder.create<SubfieldOp>(val, i);
          emitInvalidate(subfield,
                         subfield.isFieldFlipped() ? swapFlow(flow) : flow);
        }
      })
      .Case<FVectorType>([&](auto tpe) {
        auto tpex = tpe.getElementType();
        for (size_t i = 0, e = tpe.getNumElements(); i != e; ++i)
          emitInvalidate(builder.create<SubindexOp>(tpex, val, i), flow);
      })
      // Drop invalidation of analog.
      .Case<AnalogType>([](auto) {})
      // Invalidate any sink or duplex flow ground types.
      .Default([&](auto tpe) {
        if (flow != Flow::Source)
          builder.create<ConnectOp>(val, builder.create<InvalidValueOp>(tpe));
      });
}

//===-------------------------------
// FIRStmtParser Expression Parsing.

/// Parse the 'exp' grammar, returning all of the suboperations in the
/// specified vector, and the ultimate SSA value in value.
///
///  exp ::= id    // Ref
///      ::= prim
///      ::= integer-literal-exp
///      ::= exp '.' fieldId
///      ::= exp '[' intLit ']'
/// XX   ::= exp '.' DoubleLit // TODO Workaround for #470
///      ::= exp '[' exp ']'
///
///
/// If 'isLeadingStmt' is true, then this is being called to parse the first
/// expression in a statement.  We can handle some weird cases due to this if
/// we end up parsing the whole statement.  In that case we return success, but
/// set the 'result' value to null.
ParseResult FIRStmtParser::parseExpImpl(Value &result, const Twine &message,
                                        bool isLeadingStmt) {
  switch (getToken().getKind()) {

    // Handle all the primitive ops: primop exp* intLit*  ')'.  There is a
    // redundant definition of TOK_LPKEYWORD_PRIM which is needed to to get
    // around a bug in the MSVC preprocessor to properly paste together the
    // tokens lp_##SPELLING.
#define TOK_LPKEYWORD(SPELLING) case FIRToken::lp_##SPELLING:
#define TOK_LPKEYWORD_PRIM(SPELLING, CLASS, NUMOPERANDS)                       \
  case FIRToken::lp_##SPELLING:
#include "FIRTokenKinds.def"
    if (parsePrimExp(result))
      return failure();
    break;

  case FIRToken::kw_UInt:
  case FIRToken::kw_SInt:
    if (parseIntegerLiteralExp(result))
      return failure();
    break;

    // Otherwise there are a bunch of keywords that are treated as identifiers
    // try them.
  case FIRToken::identifier: // exp ::= id
  default: {
    StringRef name;
    auto loc = getToken().getLoc();
    SymbolValueEntry symtabEntry;
    if (parseId(name, message) ||
        moduleContext.lookupSymbolEntry(symtabEntry, name, loc))
      return failure();

    // If we looked up a normal value, then we're done.
    if (!moduleContext.resolveSymbolEntry(result, symtabEntry, loc, false))
      break;

    assert(symtabEntry.is<UnbundledID>() && "should be an instance");

    // Otherwise we referred to an implicitly bundled value.  We *must* be in
    // the midst of processing a field ID reference or 'is invalid'.  If not,
    // this is an error.
    if (isLeadingStmt && consumeIf(FIRToken::kw_is)) {
      if (parseToken(FIRToken::kw_invalid, "expected 'invalid'") ||
          parseOptionalInfo())
        return failure();

      locationProcessor.setLoc(loc);
      // Invalidate all of the results of the bundled value.
      unsigned unbundledId = symtabEntry.get<UnbundledID>() - 1;
      UnbundledValueEntry &ubEntry =
          moduleContext.getUnbundledEntry(unbundledId);
      for (auto elt : ubEntry)
        emitInvalidate(elt.second);

      // Signify that we parsed the whole statement.
      result = Value();
      return success();
    }

    // Handle the normal "instance.x" reference.
    StringRef fieldName;
    if (parseToken(FIRToken::period, "expected '.' in field reference") ||
        parseFieldId(fieldName, "expected field name") ||
        moduleContext.resolveSymbolEntry(result, symtabEntry, fieldName, loc))
      return failure();
    break;
  }
  }

  return parseOptionalExpPostscript(result);
}

/// Parse the postfix productions of expression after the leading expression
/// has been parsed.
///
///  exp ::= exp '.' fieldId
///      ::= exp '[' intLit ']'
/// XX   ::= exp '.' DoubleLit // TODO Workaround for #470
///      ::= exp '[' exp ']'
ParseResult FIRStmtParser::parseOptionalExpPostscript(Value &result) {

  // Handle postfix expressions.
  while (1) {
    // Subfield: exp ::= exp '.' fieldId
    if (consumeIf(FIRToken::period)) {
      if (parsePostFixFieldId(result))
        return failure();

      continue;
    }

    // Subindex: exp ::= exp '[' intLit ']' | exp '[' exp ']'
    if (consumeIf(FIRToken::l_square)) {
      if (getToken().isAny(FIRToken::integer, FIRToken::string)) {
        if (parsePostFixIntSubscript(result))
          return failure();
        continue;
      }
      if (parsePostFixDynamicSubscript(result))
        return failure();

      continue;
    }

    return success();
  }
}

/// exp ::= exp '.' fieldId
///
/// The "exp '.'" part of the production has already been parsed.
///
ParseResult FIRStmtParser::parsePostFixFieldId(Value &result) {
  auto loc = getToken().getLoc();
  StringRef fieldName;
  if (parseFieldId(fieldName, "expected field name"))
    return failure();
  auto indexV = result.getType().cast<BundleType>().getElementIndex(fieldName);
  if (!indexV)
    return emitError("unknown field '" + fieldName + "' in bundle type ")
               << result.getType(),
           failure();
  auto index = indexV.getValue();
  // Make sure the field name matches up with the input value's type and
  // compute the result type for the expression.
  NamedAttribute attrs = {getConstants().fieldIndexIdentifier,
                          builder.getUI32IntegerAttr(index)};
  auto resultType = SubfieldOp::inferReturnType({result}, attrs, {});
  if (!resultType) {
    // Emit the error at the right location.  translateLocation is expensive.
    (void)SubfieldOp::inferReturnType({result}, attrs, translateLocation(loc));
    return failure();
  }

  // Create the result operation.
  locationProcessor.setLoc(loc);
  auto op = builder.create<SubfieldOp>(resultType, result, index);
  result = op.getResult();
  return success();
}

/// exp ::= exp '[' intLit ']'
///
/// The "exp '['" part of the production has already been parsed.
///
ParseResult FIRStmtParser::parsePostFixIntSubscript(Value &result) {
  auto loc = getToken().getLoc();
  int64_t indexNo;
  if (parseIntLit(indexNo, "expected index") ||
      parseToken(FIRToken::r_square, "expected ']'"))
    return failure();

  if (indexNo < 0)
    return emitError(loc, "invalid index specifier"), failure();

  // Make sure the index expression is valid and compute the result type for the
  // expression.
  // TODO: This should ideally be folded into a `tryCreate` method on the
  // builder (https://llvm.discourse.group/t/3504).
  NamedAttribute attrs = {getConstants().indexIdentifier,
                          builder.getI32IntegerAttr(indexNo)};
  auto resultType = SubindexOp::inferReturnType({result}, attrs, {});
  if (!resultType) {
    // Emit the error at the right location.  translateLocation is expensive.
    (void)SubindexOp::inferReturnType({result}, attrs, translateLocation(loc));
    return failure();
  }

  // Create the result operation.
  locationProcessor.setLoc(loc);
  auto op = builder.create<SubindexOp>(resultType, result, attrs);
  result = op.getResult();
  return success();
}

/// exp ::= exp '[' exp ']'
///
/// The "exp '['" part of the production has already been parsed.
///
ParseResult FIRStmtParser::parsePostFixDynamicSubscript(Value &result) {
  auto loc = getToken().getLoc();
  Value index;
  if (parseExp(index, "expected subscript index expression") ||
      parseToken(FIRToken::r_square, "expected ']' in subscript"))
    return failure();

  // If the index expression is a flip type, strip it off.
  auto indexType = index.getType().cast<FIRRTLType>();
  indexType = indexType.getPassiveType();
  locationProcessor.setLoc(loc);

  // Make sure the index expression is valid and compute the result type for the
  // expression.
  auto resultType = SubaccessOp::inferReturnType({result, index}, {}, {});
  if (!resultType) {
    // Emit the error at the right location.  translateLocation is expensive.
    (void)SubaccessOp::inferReturnType({result, index}, {},
                                       translateLocation(loc));
    return failure();
  }

  // Create the result operation.
  auto op = builder.create<SubaccessOp>(resultType, result, index);
  result = op.getResult();
  return success();
}

/// prim ::= primop exp* intLit*  ')'
ParseResult FIRStmtParser::parsePrimExp(Value &result) {
  auto kind = getToken().getKind();
  auto loc = getToken().getLoc();
  consumeToken();

  // Parse the operands and constant integer arguments.
  SmallVector<Value, 4> operands;
  SmallVector<int64_t, 4> integers;
  if (parseListUntil(FIRToken::r_paren, [&]() -> ParseResult {
        // Handle the integer constant case if present.
        if (getToken().isAny(FIRToken::integer, FIRToken::signed_integer,
                             FIRToken::string)) {
          integers.push_back(0);
          return parseIntLit(integers.back(), "expected integer");
        }

        // Otherwise it must be a value operand.  These must all come before the
        // integers.
        if (!integers.empty())
          return emitError("expected more integer constants"), failure();

        Value operand;
        if (parseExp(operand, "expected expression in primitive operand"))
          return failure();

        locationProcessor.setLoc(loc);

        operands.push_back(operand);
        return success();
      }))
    return failure();

  locationProcessor.setLoc(loc);

  SmallVector<FIRRTLType, 4> opTypes;
  for (auto v : operands)
    opTypes.push_back(v.getType().cast<FIRRTLType>());

  unsigned numOperandsExpected;
  SmallVector<Identifier, 2> attrNames;

  // Get information about the primitive in question.
  switch (kind) {
  default:
    emitError(loc, "primitive not supported yet");
    return failure();
  case FIRToken::lp_validif:
    numOperandsExpected = 2;
    break;
#define TOK_LPKEYWORD_PRIM(SPELLING, CLASS, NUMOPERANDS)                       \
  case FIRToken::lp_##SPELLING:                                                \
    numOperandsExpected = NUMOPERANDS;                                         \
    break;
#include "FIRTokenKinds.def"
  }
  // Don't add code here, we want these two switch statements to be fused by
  // the compiler.
  switch (kind) {
  default:
    break;
  case FIRToken::lp_bits:
    attrNames.push_back(getConstants().hiIdentifier); // "hi"
    attrNames.push_back(getConstants().loIdentifier); // "lo"
    break;
  case FIRToken::lp_head:
  case FIRToken::lp_pad:
  case FIRToken::lp_shl:
  case FIRToken::lp_shr:
  case FIRToken::lp_tail:
    attrNames.push_back(getConstants().amountIdentifier);
    break;
  }

  if (operands.size() != numOperandsExpected) {
    assert(numOperandsExpected <= 3);
    const char *numberName[] = {"zero", "one", "two", "three"};
    const char *optionalS = &"s"[numOperandsExpected == 1];
    return emitError(loc, "operation requires ")
           << numberName[numOperandsExpected] << " operand" << optionalS;
  }

  if (integers.size() != attrNames.size()) {
    emitError(loc, "expected ") << attrNames.size() << " constant arguments";
    return failure();
  }

  NamedAttrList attrs;
  for (size_t i = 0, e = attrNames.size(); i != e; ++i)
    attrs.append(attrNames[i], builder.getI32IntegerAttr(integers[i]));

  switch (kind) {
  default:
    emitError(loc, "primitive not supported yet");
    return failure();

#define TOK_LPKEYWORD_PRIM(SPELLING, CLASS, NUMOPERANDS)                       \
  case FIRToken::lp_##SPELLING: {                                              \
    auto resultTy = CLASS::inferReturnType(operands, attrs, {});               \
    if (!resultTy) {                                                           \
      /* only call translateLocation on an error case, it is expensive. */     \
      (void)CLASS::validateAndInferReturnType(operands, attrs,                 \
                                              translateLocation(loc));         \
      return failure();                                                        \
    }                                                                          \
    result = builder.create<CLASS>(resultTy, operands, attrs);                 \
    return success();                                                          \
  }
#include "FIRTokenKinds.def"

  // Expand `validif(a, b)` expressions to `mux(a, b, invalidvalue)`, since we
  // do not provide an operation for `validif`. See docs/RationaleFIRRTL.md for
  // more details on this.
  case FIRToken::lp_validif: {
    if (opTypes.size() != 2 || !integers.empty()) {
      emitError(loc, "operation requires two operands and no constants");
      return failure();
    }
    auto lhsUInt = opTypes[0].dyn_cast<UIntType>();
    if (!lhsUInt) {
      emitError(loc, "first operand should have UInt type");
      return failure();
    }
    auto lhsWidth = lhsUInt.getWidthOrSentinel();
    if (lhsWidth != -1 && lhsWidth != 1) {
      emitError(loc, "first operand should have 'uint<1>' type");
      return failure();
    }

    // We always skip emitting the validif mux because it always folds to the
    // non-invalid operand.
    result = operands[1];
    return success();
  }
  }

  llvm_unreachable("all cases should return");
}

/// integer-literal-exp ::= 'UInt' optional-width '(' intLit ')'
///                     ::= 'SInt' optional-width '(' intLit ')'
ParseResult FIRStmtParser::parseIntegerLiteralExp(Value &result) {
  bool isSigned = getToken().is(FIRToken::kw_SInt);
  auto loc = getToken().getLoc();
  consumeToken();

  // Parse a width specifier if present.
  int64_t width;
  APInt value;
  if (parseOptionalWidth(width) ||
      parseToken(FIRToken::l_paren, "expected '(' in integer expression") ||
      parseIntLit(value, "expected integer value") ||
      parseToken(FIRToken::r_paren, "expected ')' in integer expression"))
    return failure();

  if (width == 0)
    return emitError(loc, "zero bit constants are not allowed"), failure();

  // Construct an integer attribute of the right width.
  auto type = IntType::get(builder.getContext(), isSigned, width);

  IntegerType::SignednessSemantics signedness;
  if (type.isSigned()) {
    signedness = IntegerType::Signed;
    if (width != -1) {
      // Check for overlow if we are truncating bits.
      if (unsigned(width) < value.getBitWidth() &&
          value.getNumSignBits() <= value.getBitWidth() - width) {
        return emitError(loc, "initializer too wide for declared width"),
               failure();
      }

      value = value.sextOrTrunc(width);
    }
  } else {
    signedness = IntegerType::Unsigned;
    if (width != -1) {
      // Check for overlow if we are truncating bits.
      if (unsigned(width) < value.getBitWidth() &&
          value.countLeadingZeros() < value.getBitWidth() - width) {
        return emitError(loc, "initializer too wide for declared width"),
               failure();
      }
      value = value.zextOrTrunc(width);
    }
  }

  Type attrType =
      IntegerType::get(type.getContext(), value.getBitWidth(), signedness);
  auto attr = builder.getIntegerAttr(attrType, value);

  // Check to see if we've already created this constant.  If so, reuse it.
  auto &entry = moduleContext.constantCache[{attr, type}];
  if (entry) {
    // If we already had an entry, reuse it.
    result = entry;
    return success();
  }

  // Make sure to insert constants at the top level of the module to maintain
  // dominance.
  OpBuilder::InsertPoint savedIP;

  auto parentOp = builder.getInsertionBlock()->getParentOp();
  if (!isa<FModuleOp>(parentOp)) {
    savedIP = builder.saveInsertionPoint();
    while (!isa<FModuleOp>(parentOp)) {
      builder.setInsertionPoint(parentOp);
      parentOp = builder.getInsertionBlock()->getParentOp();
    }
  }

  locationProcessor.setLoc(loc);
  auto op = builder.create<ConstantOp>(type, value);
  entry = op;
  result = op;

  if (savedIP.isSet())
    builder.setInsertionPoint(savedIP.getBlock(), savedIP.getPoint());
  return success();
}

/// The .fir grammar has the annoying property where:
/// 1) some statements start with keywords
/// 2) some start with an expression
/// 3) it allows the 'reference' expression to either be an identifier or a
///    keyword.
///
/// One example of this is something like, where this is not a register decl:
///   reg <- thing
///
/// Solving this requires lookahead to the second token.  We handle it by
///  factoring the lookahead inline into the code to keep the parser fast.
///
/// As such, statements that start with a leading keyword call this method to
/// check to see if the keyword they consumed was actually the start of an
/// expression.  If so, they parse the expression-based statement and return the
/// parser result.  If not, they return None and the statement is parsed like
/// normal.
Optional<ParseResult>
FIRStmtParser::parseExpWithLeadingKeyword(FIRToken keyword) {
  switch (getToken().getKind()) {
  default:
    // This isn't part of an expression, and isn't part of a statement.
    return None;

  case FIRToken::period:     // exp `.` identifier
  case FIRToken::l_square:   // exp `[` index `]`
  case FIRToken::kw_is:      // exp is invalid
  case FIRToken::less_equal: // exp <= thing
  case FIRToken::less_minus: // exp <- thing
    break;
  }

  Value lhs;
  SymbolValueEntry symtabEntry;
  auto loc = keyword.getLoc();

  if (moduleContext.lookupSymbolEntry(symtabEntry, keyword.getSpelling(), loc))
    return ParseResult(failure());

  // If we have a '.', we might have a symbol or an expanded port.  If we
  // resolve to a symbol, use that, otherwise check for expanded bundles of
  // other ops.
  // Non '.' ops take the plain symbol path.
  if (moduleContext.resolveSymbolEntry(lhs, symtabEntry, loc, false)) {
    // Ok if the base name didn't resolve by itself, it might be part of an
    // expanded dot reference.  That doesn't work then we fail.
    if (!consumeIf(FIRToken::period))
      return ParseResult(failure());

    StringRef fieldName;
    if (parseFieldId(fieldName, "expected field name") ||
        moduleContext.resolveSymbolEntry(lhs, symtabEntry, fieldName, loc))
      return ParseResult(failure());
  }

  // Parse any further trailing things like "mem.x.y".
  if (parseOptionalExpPostscript(lhs))
    return ParseResult(failure());

  return parseLeadingExpStmt(lhs);
}
//===-----------------------------
// FIRStmtParser Statement Parsing

/// simple_stmt_block ::= simple_stmt*
ParseResult FIRStmtParser::parseSimpleStmtBlock(unsigned indent) {
  while (true) {
    // The outer level parser can handle these tokens.
    if (getToken().isAny(FIRToken::eof, FIRToken::error))
      return success();

    auto subIndent = getIndentation();
    if (!subIndent.hasValue())
      return emitError("expected statement to be on its own line"), failure();

    if (subIndent.getValue() <= indent)
      return success();

    // Let the statement parser handle this.
    if (parseSimpleStmt(subIndent.getValue()))
      return failure();
  }
}

ParseResult FIRStmtParser::parseSimpleStmt(unsigned stmtIndent) {
  locationProcessor.startStatement();
  auto result = parseSimpleStmtImpl(stmtIndent);
  locationProcessor.endStatement(*this);
  return result;
}

/// simple_stmt ::= stmt
///
/// stmt ::= attach
///      ::= memport
///      ::= printf
///      ::= skip
///      ::= stop
///      ::= when
///      ::= leading-exp-stmt
///
/// stmt ::= instance
///      ::= cmem | smem | mem
///      ::= node | wire
///      ::= register
///
ParseResult FIRStmtParser::parseSimpleStmtImpl(unsigned stmtIndent) {
  switch (getToken().getKind()) {
  // Statements.
  case FIRToken::kw_attach:
    return parseAttach();
  case FIRToken::kw_infer:
    return parseMemPort(MemDirAttr::Infer);
  case FIRToken::kw_read:
    return parseMemPort(MemDirAttr::Read);
  case FIRToken::kw_write:
    return parseMemPort(MemDirAttr::Write);
  case FIRToken::kw_rdwr:
    return parseMemPort(MemDirAttr::ReadWrite);
  case FIRToken::lp_printf:
    return parsePrintf();
  case FIRToken::kw_skip:
    return parseSkip();
  case FIRToken::lp_stop:
    return parseStop();
  case FIRToken::lp_assert:
    return parseAssert();
  case FIRToken::lp_assume:
    return parseAssume();
  case FIRToken::lp_cover:
    return parseCover();
  case FIRToken::kw_when:
    return parseWhen(stmtIndent);
  default: {
    // Statement productions that start with an expression.
    Value lhs;
    if (parseExpLeadingStmt(lhs, "unexpected token in module"))
      return failure();
    // We use parseExp in a special mode that can complete the entire stmt at
    // once in unusual cases.  If this happened, then we are done.
    if (!lhs)
      return success();

    return parseLeadingExpStmt(lhs);
  }

    // Declarations
  case FIRToken::kw_inst:
    return parseInstance();
  case FIRToken::kw_cmem:
    return parseCMem();
  case FIRToken::kw_smem:
    return parseSMem();
  case FIRToken::kw_mem:
    return parseMem(stmtIndent);
  case FIRToken::kw_node:
    return parseNode();
  case FIRToken::kw_wire:
    return parseWire();
  case FIRToken::kw_reg:
    return parseRegister(stmtIndent);
  }
}

/// attach ::= 'attach' '(' exp+ ')' info?
ParseResult FIRStmtParser::parseAttach() {
  auto startTok = consumeToken(FIRToken::kw_attach);

  // If this was actually the start of a connect or something else handle that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  if (parseToken(FIRToken::l_paren, "expected '(' after attach"))
    return failure();

  SmallVector<Value, 4> operands;
  do {
    operands.push_back({});
    if (parseExp(operands.back(), "expected operand in attach"))
      return failure();
  } while (!consumeIf(FIRToken::r_paren));

  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  builder.create<AttachOp>(operands);
  return success();
}

/// stmt ::= mdir 'mport' id '=' id '[' exp ']' exp info?
/// mdir ::= 'infer' | 'read' | 'write' | 'rdwr'
///
ParseResult FIRStmtParser::parseMemPort(MemDirAttr direction) {
  auto mdirIndent = getIndentation();
  auto startTok = consumeToken();
  auto startLoc = startTok.getLoc();

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  StringRef memName;
  SymbolValueEntry memorySym;
  Value memory, indexExp, clock;
  if (parseToken(FIRToken::kw_mport, "expected 'mport' in memory port") ||
      parseId(id, "expected result name") ||
      parseToken(FIRToken::equal, "expected '=' in memory port") ||
      parseId(memName, "expected memory name") ||
      moduleContext.lookupSymbolEntry(memorySym, memName, startLoc) ||
      moduleContext.resolveSymbolEntry(memory, memorySym, startLoc) ||
      parseToken(FIRToken::l_square, "expected '[' in memory port") ||
      parseExp(indexExp, "expected index expression") ||
      parseToken(FIRToken::r_square, "expected ']' in memory port") ||
      parseExp(clock, "expected clock expression") || parseOptionalInfo())
    return failure();

  auto memVType = memory.getType().dyn_cast<FVectorType>();
  if (!memVType)
    return emitError(startLoc, "memory should have vector type");
  auto resultType = memVType.getElementType();

  auto annotations = getAnnotations(getModuleTarget() + ">" + id, startLoc,
                                    moduleContext.targetsInModule, resultType);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  locationProcessor.setLoc(startLoc);
  auto resultMemoryPort = builder.create<MemoryPortOp>(
      resultType, memory, indexExp, clock, direction, name, annotations);
  Value result = resultMemoryPort;

  // TODO(firrtl scala bug): If the next operation is a skip, just eat it if it
  // is at the same indent level as us.  This is a horrible hack on top of the
  // following hack to work around a Scala bug.
  auto nextIndent = getIndentation();
  if (getToken().is(FIRToken::kw_skip) && mdirIndent.hasValue() &&
      nextIndent.hasValue() && mdirIndent.getValue() == nextIndent.getValue()) {
    // End the location for the MemPort, and start the location processing for
    // the skip op.
    locationProcessor.endStatement(*this);
    locationProcessor.startStatement();
    if (parseSkip())
      return failure();

    nextIndent = getIndentation();
  }

  // TODO(firrtl scala bug): Chisel is creating invalid IR where the mports
  // are logically defining their name at the scope of the memory itself.  This
  // is a problem for us, because the index expression and clock may be defined
  // in a nested expression.  We can't really tell when this is happening
  //  without unbounded lookahead either.
  //
  // Fortunately, this seems to happen in very idiomatic cases, where the
  // mport happens at the end of a when block.  We detect this situation by
  // seeing if the next statement is indented less than our memport - if so,
  // this is the last statement at the end of the 'when' block.   Trigger a
  // hacky workaround just in this case.
  bool insertNameIntoGlobalScope = false;
  if (mdirIndent.hasValue() && nextIndent.hasValue() &&
      mdirIndent.getValue() > nextIndent.getValue() &&
      !isa<FModuleOp>(resultMemoryPort->getParentOp())) {

    // If we need to inject this name into a parent scope, then we have to do
    // some IR hackery.  Create a wire for the id name right before
    // the mem in question, inject its name into that scope, then connect
    // the output of the mport to it.
    WireOp wireHack;
    /* inject the wire into the enclosing firrtl.module*/ {
      // TODO: Store this in moduleContext.
      auto curModule = resultMemoryPort->getParentOfType<FModuleOp>();
      auto insertPoint = builder.saveInsertionPoint();
      builder.setInsertionPointToStart(curModule.getBodyBlock());
      wireHack = builder.create<WireOp>(result.getType());
      builder.restoreInsertionPoint(insertPoint);
    }
    builder.create<ConnectOp>(wireHack, result);

    // Inject this the wire's name into the global scope.
    result = wireHack;
    insertNameIntoGlobalScope = true;
  }

  return moduleContext.addSymbolEntry(id, result, startLoc,
                                      insertNameIntoGlobalScope);
}

/// printf ::= 'printf(' exp exp StringLit exp* ')' name? info?
ParseResult FIRStmtParser::parsePrintf() {
  auto startTok = consumeToken(FIRToken::lp_printf);

  Value clock, condition;
  StringRef formatString;
  if (parseExp(clock, "expected clock expression in printf") ||
      parseExp(condition, "expected condition in printf") ||
      parseGetSpelling(formatString) ||
      parseToken(FIRToken::string, "expected format string in printf"))
    return failure();

  SmallVector<Value, 4> operands;
  while (!consumeIf(FIRToken::r_paren)) {
    operands.push_back({});
    if (parseExp(operands.back(), "expected operand in printf"))
      return failure();
  }

  StringAttr name;
  if (parseOptionalName(name))
    return failure();

  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  auto formatStrUnescaped = FIRToken::getStringValue(formatString);
  StringRef formatStringRef(formatStrUnescaped);
  if (formatStringRef.startswith("cover:")) {
    APInt constOne(1, 1, false);
    auto constTrue =
        builder.create<ConstantOp>(UIntType::get(getContext(), 1), constOne);
    builder.create<CoverOp>(clock, condition, constTrue, formatStrUnescaped,
                            "");
    return success();
  }
  if (formatStringRef.startswith("assert:")) {
    APInt constOne(1, 1, false);
    auto constTrue =
        builder.create<ConstantOp>(UIntType::get(getContext(), 1), constOne);
    builder.create<AssertOp>(clock, condition, constTrue, formatStrUnescaped,
                             "");
    return success();
  }
  if (formatStringRef.startswith("assume:")) {
    APInt constOne(1, 1, false);
    auto constTrue =
        builder.create<ConstantOp>(UIntType::get(getContext(), 1), constOne);
    builder.create<AssumeOp>(clock, condition, constTrue, formatStrUnescaped,
                             "");
    return success();
  }

  builder.create<PrintFOp>(clock, condition,
                           builder.getStringAttr(formatStrUnescaped), operands,
                           name);
  return success();
}

/// skip ::= 'skip' info?
ParseResult FIRStmtParser::parseSkip() {
  auto startTok = consumeToken(FIRToken::kw_skip);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  builder.create<SkipOp>();
  return success();
}

/// stop ::= 'stop(' exp exp intLit ')' info?
ParseResult FIRStmtParser::parseStop() {
  auto startTok = consumeToken(FIRToken::lp_stop);

  Value clock, condition;
  int64_t exitCode;
  StringAttr name;
  if (parseExp(clock, "expected clock expression in 'stop'") ||
      parseExp(condition, "expected condition in 'stop'") ||
      parseIntLit(exitCode, "expected exit code in 'stop'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'stop'") ||
      parseOptionalName(name) || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  builder.create<StopOp>(clock, condition, builder.getI32IntegerAttr(exitCode),
                         name);
  return success();
}

/// assert ::= 'assert(' exp exp exp StringLit ')' info?
ParseResult FIRStmtParser::parseAssert() {
  auto startTok = consumeToken(FIRToken::lp_assert);

  Value clock, predicate, enable;
  StringRef message;
  StringAttr name;
  if (parseExp(clock, "expected clock expression in 'assert'") ||
      parseExp(predicate, "expected predicate in 'assert'") ||
      parseExp(enable, "expected enable in 'assert'") ||
      parseGetSpelling(message) ||
      parseToken(FIRToken::string, "expected message in 'assert'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'assert'") ||
      parseOptionalName(name) || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  auto messageUnescaped = FIRToken::getStringValue(message);
  builder.create<AssertOp>(clock, predicate, enable,
                           builder.getStringAttr(messageUnescaped), name);
  return success();
}

/// assume ::= 'assume(' exp exp exp StringLit ')' info?
ParseResult FIRStmtParser::parseAssume() {
  auto startTok = consumeToken(FIRToken::lp_assume);

  Value clock, predicate, enable;
  StringRef message;
  StringAttr name;
  if (parseExp(clock, "expected clock expression in 'assume'") ||
      parseExp(predicate, "expected predicate in 'assume'") ||
      parseExp(enable, "expected enable in 'assume'") ||
      parseGetSpelling(message) ||
      parseToken(FIRToken::string, "expected message in 'assume'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'assume'") ||
      parseOptionalName(name) || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  auto messageUnescaped = FIRToken::getStringValue(message);
  builder.create<AssumeOp>(clock, predicate, enable,
                           builder.getStringAttr(messageUnescaped), name);
  return success();
}

/// cover ::= 'cover(' exp exp exp StringLit ')' info?
ParseResult FIRStmtParser::parseCover() {
  auto startTok = consumeToken(FIRToken::lp_cover);

  Value clock, predicate, enable;
  StringRef message;
  StringAttr name;
  if (parseExp(clock, "expected clock expression in 'cover'") ||
      parseExp(predicate, "expected predicate in 'cover'") ||
      parseExp(enable, "expected enable in 'cover'") ||
      parseGetSpelling(message) ||
      parseToken(FIRToken::string, "expected message in 'cover'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'cover'") ||
      parseOptionalName(name) || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  auto messageUnescaped = FIRToken::getStringValue(message);
  builder.create<CoverOp>(clock, predicate, enable,
                          builder.getStringAttr(messageUnescaped), name);
  return success();
}

/// when  ::= 'when' exp ':' info? suite? ('else' ( when | ':' info? suite?)
/// )? suite ::= simple_stmt | INDENT simple_stmt+ DEDENT
ParseResult FIRStmtParser::parseWhen(unsigned whenIndent) {
  auto startTok = consumeToken(FIRToken::kw_when);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  Value condition;
  if (parseExp(condition, "expected condition in 'when'") ||
      parseToken(FIRToken::colon, "expected ':' in when") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  // Create the IR representation for the when.
  auto whenStmt = builder.create<WhenOp>(condition, /*createElse*/ false);

  // This is a function to parse a suite body.
  auto parseSuite = [&](Block &blockToInsertInto) -> ParseResult {
    // Declarations within the suite are scoped to within the suite.
    FIRModuleContext::SymbolTableScope suiteScope(moduleContext);

    // After parsing the when region, we can release any new entries in
    // unbundledValues since the symbol table entries that refer to them will be
    // gone.
    struct UnbundledValueRestorer {
      UnbundledValuesList &list;
      size_t startingSize;
      UnbundledValueRestorer(UnbundledValuesList &list) : list(list) {
        startingSize = list.size();
      }
      ~UnbundledValueRestorer() { list.resize(startingSize); }
    } x(moduleContext.unbundledValues);

    // We parse the substatements into their own parser, so they get inserted
    // into the specified 'when' region.
    FIRStmtParser subParser(blockToInsertInto, moduleContext);

    // Figure out whether the body is a single statement or a nested one.
    auto stmtIndent = getIndentation();

    // Parsing a single statment is straightforward.
    if (!stmtIndent.hasValue())
      return subParser.parseSimpleStmt(whenIndent);

    if (stmtIndent.getValue() <= whenIndent)
      return emitError("statement must be indented more than 'when'"),
             failure();

    // Parse a block of statements that are indented more than the when.
    return subParser.parseSimpleStmtBlock(whenIndent);
  };

  // Parse the 'then' body into the 'then' region.
  if (parseSuite(whenStmt.getThenBlock()))
    return failure();

  // If the else is present, handle it otherwise we're done.
  if (getToken().isNot(FIRToken::kw_else))
    return success();

  // If the 'else' is less indented than the when, then it must belong to some
  // containing 'when'.
  auto elseIndent = getIndentation();
  if (elseIndent.hasValue() && elseIndent.getValue() < whenIndent)
    return success();

  consumeToken(FIRToken::kw_else);

  // Create an else block to parse into.
  whenStmt.createElseRegion();

  // If we have the ':' form, then handle it.

  // Syntactic shorthand 'else when'. This uses the same indentation level as
  // the outer 'when'.
  if (getToken().is(FIRToken::kw_when)) {
    // We create a sub parser for the else block.
    FIRStmtParser subParser(whenStmt.getElseBlock(), moduleContext);
    return subParser.parseSimpleStmt(whenIndent);
  }

  // Parse the 'else' body into the 'else' region.
  LocationAttr elseLoc; // ignore the else locator.
  if (parseToken(FIRToken::colon, "expected ':' after 'else'") ||
      parseOptionalInfoLocator(elseLoc) || parseSuite(whenStmt.getElseBlock()))
    return failure();

  // TODO(firrtl spec): There is no reason for the 'else :' grammar to take an
  // info.  It doesn't appear to be generated either.
  return success();
}

/// leading-exp-stmt ::= exp '<=' exp info?
///                  ::= exp '<-' exp info?
///                  ::= exp 'is' 'invalid' info?
ParseResult FIRStmtParser::parseLeadingExpStmt(Value lhs) {
  auto loc = getToken().getLoc();

  // If 'is' grammar is special.
  if (consumeIf(FIRToken::kw_is)) {
    if (parseToken(FIRToken::kw_invalid, "expected 'invalid'") ||
        parseOptionalInfo())
      return failure();

    locationProcessor.setLoc(loc);
    emitInvalidate(lhs);
    return success();
  }

  auto kind = getToken().getKind();
  if (getToken().isNot(FIRToken::less_equal, FIRToken::less_minus))
    return emitError("expected '<=', '<-', or 'is' in statement"), failure();
  consumeToken();

  Value rhs;
  if (parseExp(rhs, "unexpected token in statement") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(loc);

  if (kind == FIRToken::less_equal) {
    // Some operations, dshl for example, have implicit truncations, even in lo
    // firrtl.  Chisel will also use connects as partial connects to do
    // truncation.  Handle truncations as partial connects, which allow
    // truncation.
    auto lhsPType = lhs.getType().cast<FIRRTLType>().getPassiveType();
    auto rhsPType = rhs.getType().cast<FIRRTLType>().getPassiveType();
    if (lhsPType != rhsPType && lhsPType.getBitWidthOrSentinel() >= 0 &&
        lhsPType.getBitWidthOrSentinel() < rhsPType.getBitWidthOrSentinel()) {
      builder.create<PartialConnectOp>(lhs, rhs);
    } else {
      builder.create<ConnectOp>(lhs, rhs);
    }
  } else {
    assert(kind == FIRToken::less_minus && "unexpected kind");
    builder.create<PartialConnectOp>(lhs, rhs);
  }
  return success();
}

//===-------------------------------
// FIRStmtParser Declaration Parsing

/// instance ::= 'inst' id 'of' id info?
ParseResult FIRStmtParser::parseInstance() {
  auto startTok = consumeToken(FIRToken::kw_inst);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  StringRef moduleName;
  if (parseId(id, "expected instance name") ||
      parseToken(FIRToken::kw_of, "expected 'of' in instance") ||
      parseId(moduleName, "expected module name") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  // Look up the module that is being referenced.
  auto circuit =
      builder.getBlock()->getParentOp()->getParentOfType<CircuitOp>();
  auto referencedModule = circuit.lookupSymbol(moduleName);
  if (!referencedModule) {
    emitError(startTok.getLoc(),
              "use of undefined module name '" + moduleName + "' in instance");
    return failure();
  }

  SmallVector<ModulePortInfo> modulePorts = getModulePortInfo(referencedModule);

  // Make a bundle of the inputs and outputs of the specified module.
  SmallVector<Type, 4> resultTypes;
  resultTypes.reserve(modulePorts.size());
  SmallVector<std::pair<StringAttr, Type>, 4> resultNamesAndTypes;

  for (auto port : modulePorts) {
    resultTypes.push_back(port.type);
    resultNamesAndTypes.push_back({port.name, port.type});
  }

  // Combine annotations that are ReferenceTargets and InstanceTargets.  By
  // example, this will lookup all annotations with either of the following
  // formats:
  //     ~Foo|Foo>bar
  //     ~Foo|Foo/bar:Bar
  auto annotations = getSplitAnnotations(
      {getModuleTarget() + ">" + id,
       getModuleTarget() + "/" + id + ":" + moduleName},
      startTok.getLoc(), resultNamesAndTypes, moduleContext.targetsInModule);

  // Keep the name if a dont touch exist on either the instance or its ports.
  auto dontTouch = hasDontTouch(annotations.first) ||
                   llvm::any_of(annotations.second, [&](Attribute a) {
                     return hasDontTouch(a.cast<ArrayAttr>());
                   });
  auto name = dontTouch ? id : filterUselessName(id);

  auto result = builder.create<InstanceOp>(resultTypes, moduleName, name,
                                           annotations.first.getValue(),
                                           annotations.second.getValue());

  // Since we are implicitly unbundling the instance results, we need to keep
  // track of the mapping from bundle fields to results in the unbundledValues
  // data structure.  Build our entry now.
  UnbundledValueEntry unbundledValueEntry;
  unbundledValueEntry.reserve(modulePorts.size());
  for (size_t i = 0, e = modulePorts.size(); i != e; ++i)
    unbundledValueEntry.push_back({modulePorts[i].name, result.getResult(i)});

  // Add it to unbundledValues and add an entry to the symbol table to remember
  // it.
  moduleContext.unbundledValues.push_back(std::move(unbundledValueEntry));
  auto entryId = UnbundledID(moduleContext.unbundledValues.size());
  return moduleContext.addSymbolEntry(id, entryId, startTok.getLoc());
}

/// cmem ::= 'cmem' id ':' type info?
ParseResult FIRStmtParser::parseCMem() {
  // TODO(firrtl spec) cmem is completely undocumented.
  auto startTok = consumeToken(FIRToken::kw_cmem);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  if (parseId(id, "expected cmem name") ||
      parseToken(FIRToken::colon, "expected ':' in cmem") ||
      parseType(type, "expected cmem type") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  auto annotations =
      getAnnotations(getModuleTarget() + ">" + id, startTok.getLoc(),
                     moduleContext.targetsInModule, type);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result = builder.create<CMemOp>(type, name, annotations);
  return moduleContext.addSymbolEntry(id, result, startTok.getLoc());
}

/// smem ::= 'smem' id ':' type ruw? info?
ParseResult FIRStmtParser::parseSMem() {
  // TODO(firrtl spec) smem is completely undocumented.
  auto startTok = consumeToken(FIRToken::kw_smem);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  RUWAttr ruw = RUWAttr::Undefined;

  if (parseId(id, "expected cmem name") ||
      parseToken(FIRToken::colon, "expected ':' in cmem") ||
      parseType(type, "expected cmem type") || parseOptionalRUW(ruw) ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  auto annotations =
      getAnnotations(getModuleTarget() + ">" + id, startTok.getLoc(),
                     moduleContext.targetsInModule, type);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result = builder.create<SMemOp>(type, ruw, name, annotations);
  return moduleContext.addSymbolEntry(id, result, startTok.getLoc());
}

/// mem ::= 'mem' id ':' info? INDENT memField* DEDENT
/// memField ::= 'data-type' '=>' type NEWLINE
/// 	       ::= 'depth' '=>' intLit NEWLINE
/// 	       ::= 'read-latency' '=>' intLit NEWLINE
/// 	       ::= 'write-latency' '=>' intLit NEWLINE
/// 	       ::= 'read-under-write' '=>' ruw NEWLINE
/// 	       ::= 'reader' '=>' id+ NEWLINE
/// 	       ::= 'writer' '=>' id+ NEWLINE
/// 	       ::= 'readwriter' '=>' id+ NEWLINE
ParseResult FIRStmtParser::parseMem(unsigned memIndent) {
  auto startTok = consumeToken(FIRToken::kw_mem);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  if (parseId(id, "expected mem name") ||
      parseToken(FIRToken::colon, "expected ':' in mem") || parseOptionalInfo())
    return failure();

  FIRRTLType type;
  int64_t depth = -1, readLatency = -1, writeLatency = -1;
  RUWAttr ruw = RUWAttr::Undefined;

  SmallVector<std::pair<StringAttr, Type>, 4> ports;

  // Parse all the memfield records, which are indented more than the mem.
  while (1) {
    auto nextIndent = getIndentation();
    if (!nextIndent.hasValue() || nextIndent.getValue() <= memIndent)
      break;

    auto spelling = getTokenSpelling();
    if (parseToken(FIRToken::identifier, "unexpected token in 'mem'") ||
        parseToken(FIRToken::equal_greater, "expected '=>' in 'mem'"))
      return failure();

    if (spelling == "data-type") {
      if (type)
        return emitError("'mem' type specified multiple times"), failure();

      if (parseType(type, "expected type in data-type declaration"))
        return failure();
      continue;
    }
    if (spelling == "depth") {
      if (parseIntLit(depth, "expected integer in depth specification"))
        return failure();
      continue;
    }
    if (spelling == "read-latency") {
      if (parseIntLit(readLatency, "expected integer latency"))
        return failure();
      continue;
    }
    if (spelling == "write-latency") {
      if (parseIntLit(writeLatency, "expected integer latency"))
        return failure();
      continue;
    }
    if (spelling == "read-under-write") {
      if (getToken().isNot(FIRToken::kw_old, FIRToken::kw_new,
                           FIRToken::kw_undefined))
        return emitError("expected specifier"), failure();

      if (parseOptionalRUW(ruw))
        return failure();
      continue;
    }

    MemOp::PortKind portKind;
    if (spelling == "reader")
      portKind = MemOp::PortKind::Read;
    else if (spelling == "writer")
      portKind = MemOp::PortKind::Write;
    else if (spelling == "readwriter")
      portKind = MemOp::PortKind::ReadWrite;
    else
      return emitError("unexpected field in 'mem' declaration"), failure();

    StringRef portName;
    if (parseId(portName, "expected port name"))
      return failure();
    ports.push_back({builder.getStringAttr(portName),
                     MemOp::getTypeForPort(depth, type, portKind)});

    while (!getIndentation().hasValue()) {
      if (parseId(portName, "expected port name"))
        return failure();
      ports.push_back({builder.getStringAttr(portName),
                       MemOp::getTypeForPort(depth, type, portKind)});
    }
  }

  // The FIRRTL dialect requires mems to have at least one port.  Since portless
  // mems can never be referenced, it is always safe to drop them.
  if (ports.empty())
    return success();

  // Canonicalize the ports into alphabetical order.
  // TODO: Move this into MemOp construction/canonicalization.
  llvm::array_pod_sort(ports.begin(), ports.end(),
                       [](const std::pair<StringAttr, Type> *lhs,
                          const std::pair<StringAttr, Type> *rhs) -> int {
                         return lhs->first.getValue().compare(
                             rhs->first.getValue());
                       });

  SmallVector<Attribute, 4> resultNames;
  SmallVector<Type, 4> resultTypes;
  for (auto p : ports) {
    resultNames.push_back(p.first);
    resultTypes.push_back(p.second);
  }

  locationProcessor.setLoc(startTok.getLoc());

  auto annotations =
      getSplitAnnotations(getModuleTarget() + ">" + id, startTok.getLoc(),
                          ports, moduleContext.targetsInModule);

  // Keep the name if a dont touch exist on either the instance or its ports.
  auto dontTouch = hasDontTouch(annotations.first) ||
                   llvm::any_of(annotations.second, [&](Attribute a) {
                     return hasDontTouch(a.cast<ArrayAttr>());
                   });
  auto name = dontTouch ? id : filterUselessName(id);

  auto result =
      builder.create<MemOp>(resultTypes, readLatency, writeLatency, depth, ruw,
                            builder.getArrayAttr(resultNames), name,
                            annotations.first, annotations.second);

  UnbundledValueEntry unbundledValueEntry;
  unbundledValueEntry.reserve(result.getNumResults());
  for (size_t i = 0, e = result.getNumResults(); i != e; ++i)
    unbundledValueEntry.push_back({resultNames[i], result.getResult(i)});

  moduleContext.unbundledValues.push_back(std::move(unbundledValueEntry));
  auto entryID = UnbundledID(moduleContext.unbundledValues.size());
  return moduleContext.addSymbolEntry(id, entryID, startTok.getLoc());
}

/// node ::= 'node' id '=' exp info?
ParseResult FIRStmtParser::parseNode() {
  auto startTok = consumeToken(FIRToken::kw_node);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  Value initializer;
  if (parseId(id, "expected node name") ||
      parseToken(FIRToken::equal, "expected '=' in node") ||
      parseExp(initializer, "expected expression for node") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  // Error out in the following conditions:
  //
  //   1. Node type is Analog (at the top level)
  //   2. Node type is not passive under an optional outer flip
  //      (analog field is okay)
  //
  // Note: (1) is more restictive than normal NodeOp verification, but
  // this is added to align with the SFC. (2) is less restrictive than
  // the SFC to accomodate for situations where the node is something
  // weird like a module output or an instance input.
  auto initializerType = initializer.getType().cast<FIRRTLType>();
  if (initializerType.isa<AnalogType>() || !initializerType.isPassive()) {
    emitError(startTok.getLoc())
        << "Node cannot be analog and must be passive or passive under a flip "
        << initializer.getType();
    return failure();
  }

  auto annotations =
      getAnnotations(getModuleTarget() + ">" + id, startTok.getLoc(),
                     moduleContext.targetsInModule, initializerType);

  // Ignore useless names like _T.
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  // The entire point of a node declaration is to carry a name.  If it got
  // dropped, then we don't even need to create a result unless it is annotated.
  Value result;
  if (!name.empty() || !annotations.empty()) {
    result = builder.create<NodeOp>(initializer.getType(), initializer, name,
                                    annotations);
  } else
    result = initializer;
  return moduleContext.addSymbolEntry(id, result, startTok.getLoc());
}

/// wire ::= 'wire' id ':' type info?
ParseResult FIRStmtParser::parseWire() {
  auto startTok = consumeToken(FIRToken::kw_wire);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  if (parseId(id, "expected wire name") ||
      parseToken(FIRToken::colon, "expected ':' in wire") ||
      parseType(type, "expected wire type") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  auto annotations =
      getAnnotations(getModuleTarget() + ">" + id, startTok.getLoc(),
                     moduleContext.targetsInModule, type);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result = builder.create<WireOp>(type, name, annotations);
  return moduleContext.addSymbolEntry(id, result, startTok.getLoc());
}

/// register    ::= 'reg' id ':' type exp ('with' ':' reset_block)? info?
///
/// reset_block ::= INDENT simple_reset info? NEWLINE DEDENT
///             ::= '(' simple_reset ')'
///
/// simple_reset ::= simple_reset0
///              ::= '(' simple_reset0 ')'
///
/// simple_reset0:  'reset' '=>' '(' exp exp ')'
///
ParseResult FIRStmtParser::parseRegister(unsigned regIndent) {
  auto startTok = consumeToken(FIRToken::kw_reg);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  Value clock;

  // TODO(firrtl spec): info? should come after the clock expression before
  // the 'with'.
  if (parseId(id, "expected reg name") ||
      parseToken(FIRToken::colon, "expected ':' in reg") ||
      parseType(type, "expected reg type") ||
      parseExp(clock, "expected expression for register clock"))
    return failure();

  // Parse the 'with' specifier if present.
  Value resetSignal, resetValue;
  if (consumeIf(FIRToken::kw_with)) {
    if (parseToken(FIRToken::colon, "expected ':' in reg"))
      return failure();

    // TODO(firrtl spec): Simplify the grammar for register reset logic.
    // Why allow multiple ambiguous parentheses?  Why rely on indentation at
    // all?

    // This implements what the examples have in practice.
    bool hasExtraLParen = consumeIf(FIRToken::l_paren);

    auto indent = getIndentation();
    if (!indent.hasValue() || indent.getValue() <= regIndent)
      if (!hasExtraLParen)
        return emitError("expected indented reset specifier in reg"), failure();

    if (parseToken(FIRToken::kw_reset, "expected 'reset' in reg") ||
        parseToken(FIRToken::equal_greater, "expected => in reset specifier") ||
        parseToken(FIRToken::l_paren, "expected '(' in reset specifier") ||
        parseExp(resetSignal, "expected expression for reset signal"))
      return failure();

    // The Scala implementation of FIRRTL represents registers without resets
    // as a self referential register... and the pretty printer doesn't print
    // the right form. Recognize that this is happening and treat it as a
    // register without a reset for compatibility.
    // TODO(firrtl scala impl): pretty print registers without resets right.
    if (getTokenSpelling() == id) {
      consumeToken();
      if (parseToken(FIRToken::r_paren, "expected ')' in reset specifier"))
        return failure();
      resetSignal = Value();
    } else {
      if (parseExp(resetValue, "expected expression for reset value") ||
          parseToken(FIRToken::r_paren, "expected ')' in reset specifier"))
        return failure();
    }

    if (hasExtraLParen &&
        parseToken(FIRToken::r_paren, "expected ')' in reset specifier"))
      return failure();
  }

  // Finally, handle the last info if present, providing location info for the
  // clock expression.
  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  auto annotations =
      getAnnotations(getModuleTarget() + ">" + id, startTok.getLoc(),
                     moduleContext.targetsInModule, type);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  Value result;
  if (resetSignal)
    result = builder.create<RegResetOp>(type, clock, resetSignal, resetValue,
                                        name, annotations);
  else
    result = builder.create<RegOp>(type, clock, name, annotations);

  return moduleContext.addSymbolEntry(id, result, startTok.getLoc());
}

//===----------------------------------------------------------------------===//
// FIRCircuitParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements the outer level of the parser, including things
/// like circuit and module.
struct FIRCircuitParser : public FIRParser {
  explicit FIRCircuitParser(SharedParserConstants &state, FIRLexer &lexer,
                            ModuleOp mlirModule)
      : FIRParser(state, lexer), mlirModule(mlirModule) {}

  ParseResult parseCircuit(const llvm::MemoryBuffer *annotationsBuf);

private:
  /// Add annotations from a string to the internal annotation map.  Report
  /// errors using a provided source manager location and with a provided error
  /// message
  ParseResult importAnnotations(SMLoc loc, StringRef circuitTarget,
                                StringRef annotationsStr);

  ParseResult parseModule(CircuitOp circuit, StringRef circuitTarget,
                          unsigned indent);

  ParseResult parsePortList(SmallVectorImpl<ModulePortInfo> &resultPorts,
                            SmallVectorImpl<SMLoc> &resultPortLocs,
                            Location defaultLoc, StringRef moduleTarget,
                            unsigned indent);

  struct DeferredModuleToParse {
    FModuleOp moduleOp;
    SmallVector<SMLoc> portLocs;
    FIRLexerCursor lexerCursor;
    std::string moduleTarget;
    unsigned indent;
    TargetSet targetSet;
  };

  ParseResult parseModuleBody(DeferredModuleToParse &deferredModule);

  SmallVector<DeferredModuleToParse, 0> deferredModules;
  ModuleOp mlirModule;

  /// A global identifier that can be used to link multiple annotations
  /// together.  This should be incremented on use.
  unsigned annotationID = 0;
};

} // end anonymous namespace

ParseResult FIRCircuitParser::importAnnotations(SMLoc loc,
                                                StringRef circuitTarget,
                                                StringRef annotationsStr) {

  auto annotations = json::parse(annotationsStr);
  if (auto err = annotations.takeError()) {
    handleAllErrors(std::move(err), [&](const json::ParseError &a) {
      auto diag = emitError(loc, "Failed to parse JSON Annotations");
      diag.attachNote() << a.message();
    });
    return failure();
  }

  json::Path::Root root;
  llvm::StringMap<ArrayAttr> thisAnnotationMap;
  if (!fromJSON(annotations.get(), circuitTarget, thisAnnotationMap, root,
                getContext())) {
    auto diag = emitError(loc, "Invalid/unsupported annotation format");
    std::string jsonErrorMessage =
        "See inline comments for problem area in JSON:\n";
    llvm::raw_string_ostream s(jsonErrorMessage);
    root.printErrorContext(annotations.get(), s);
    diag.attachNote() << jsonErrorMessage;
    return failure();
  }

  if (!scatterCustomAnnotations(thisAnnotationMap, getContext(), annotationID,
                                translateLocation(loc)))
    return failure();

  // Merge the attributes we just parsed into the global set we're accumulating.
  llvm::StringMap<ArrayAttr> &resultAnnoMap = getConstants().annotationMap;
  for (auto &thisEntry : thisAnnotationMap) {
    auto &existing = resultAnnoMap[thisEntry.getKey()];
    if (!existing) {
      existing = thisEntry.getValue();
      continue;
    }

    SmallVector<Attribute> annotationVec(existing.begin(), existing.end());
    annotationVec.append(thisEntry.getValue().begin(),
                         thisEntry.getValue().end());
    existing = ArrayAttr::get(getContext(), annotationVec);
  }

  return success();
}

/// pohwist ::= port*
/// port     ::= dir id ':' type info? NEWLINE
/// dir      ::= 'input' | 'output'
///
/// defaultLoc specifies a location to use if there is no info locator for the
/// port.
///
ParseResult
FIRCircuitParser::parsePortList(SmallVectorImpl<ModulePortInfo> &resultPorts,
                                SmallVectorImpl<SMLoc> &resultPortLocs,
                                Location defaultLoc, StringRef moduleTarget,
                                unsigned indent) {
  // Parse any ports.
  while (getToken().isAny(FIRToken::kw_input, FIRToken::kw_output) &&
         // Must be nested under the module.
         getIndentation() > indent) {

    // We need one token lookahead to resolve the ambiguity between:
    // output foo             ; port
    // output <= input        ; identifier expression
    // output.thing <= input  ; identifier expression
    auto backtrackState = getLexer().getCursor();

    bool isOutput = getToken().is(FIRToken::kw_output);
    consumeToken();

    // If we have something that isn't a keyword then this must be an
    // identifier, not an input/output marker.
    if (!getToken().is(FIRToken::identifier) && !getToken().isKeyword()) {
      backtrackState.restore(getLexer());
      break;
    }

    StringAttr name;
    FIRRTLType type;
    LocWithInfo info(getToken().getLoc(), this);
    if (parseId(name, "expected port name") ||
        parseToken(FIRToken::colon, "expected ':' in port definition") ||
        parseType(type, "expected a type in port declaration") ||
        info.parseOptionalInfo())
      return failure();

    // Ports typically do not have locators.  We rather default to the locaiton
    // of the module rather than a location in a .fir file.  If the module had a
    // locator then this will be more friendly.  If not, this doesn't burn
    // compile time creating too many unique locations.
    info.setDefaultLoc(defaultLoc);

    AnnotationSet annotations(
        getAnnotations(moduleTarget + ">" + name.getValue(), info.getFIRLoc(),
                       getConstants().targetSet, type));

    resultPorts.push_back(
        {name, type, direction::get(isOutput), info.getLoc(), annotations});
    resultPortLocs.push_back(info.getFIRLoc());
  }

  return success();
}

/// Parse an external or present module depending on what token we have.
///
/// module ::= 'module' id ':' info? INDENT pohwist simple_stmt_block DEDENT
/// module ::=
///        'extmodule' id ':' info? INDENT pohwist defname? parameter* DEDENT
/// defname   ::= 'defname' '=' id NEWLINE
///
/// parameter ::= 'parameter' id '=' intLit NEWLINE
/// parameter ::= 'parameter' id '=' StringLit NEWLINE
/// parameter ::= 'parameter' id '=' floatingpoint NEWLINE
/// parameter ::= 'parameter' id '=' RawString NEWLINE
ParseResult FIRCircuitParser::parseModule(CircuitOp circuit,
                                          StringRef circuitTarget,
                                          unsigned indent) {
  bool isExtModule = getToken().is(FIRToken::kw_extmodule);
  consumeToken();
  StringAttr name;
  SmallVector<ModulePortInfo, 8> portList;
  SmallVector<SMLoc> portLocs;

  LocWithInfo info(getToken().getLoc(), this);
  if (parseId(name, "expected module name"))
    return failure();

  auto moduleTarget = (circuitTarget + "|" + name.getValue()).str();
  ArrayAttr annotations = getAnnotations({moduleTarget}, info.getFIRLoc(),
                                         getConstants().targetSet);

  if (parseToken(FIRToken::colon, "expected ':' in module definition") ||
      info.parseOptionalInfo() ||
      parsePortList(portList, portLocs, info.getLoc(), moduleTarget, indent))
    return failure();

  auto builder = circuit.getBodyBuilder();

  // Check for port name collisions.
  SmallDenseMap<Attribute, SMLoc> portIds;
  for (auto portAndLoc : llvm::zip(portList, portLocs)) {
    ModulePortInfo &port = std::get<0>(portAndLoc);
    auto &entry = portIds[port.name];
    if (!entry.isValid()) {
      entry = std::get<1>(portAndLoc);
      continue;
    }

    emitError(std::get<1>(portAndLoc),
              "redefinition of name '" + port.getName() + "'")
            .attachNote(translateLocation(entry))
        << "previous definition here";
    return failure();
  }

  // If this is a normal module, parse the body into an FModuleOp.
  if (!isExtModule) {
    auto moduleOp =
        builder.create<FModuleOp>(info.getLoc(), name, portList, annotations);

    // Parse the body of this module after all prototypes have been parsed. This
    // allows us to handle forward references correctly.
    deferredModules.emplace_back(
        DeferredModuleToParse{moduleOp, portLocs, getLexer().getCursor(),
                              std::move(moduleTarget), indent, TargetSet()});

    // We're going to defer parsing this module, so just skip tokens until we
    // get to the next module or the end of the file.
    while (true) {
      switch (getToken().getKind()) {

      // End of file or invalid token will be handled by outer level.
      case FIRToken::eof:
      case FIRToken::error:
        return success();

        // If we got to the next module, then we're done.
      case FIRToken::kw_module:
      case FIRToken::kw_extmodule:
        return success();

      default:
        consumeToken();
        break;
      }
    }
  }

  // Otherwise, handle extmodule specific features like parameters.

  // Parse a defname if present.
  // TODO(firrtl spec): defname isn't documented at all, what is it?
  StringRef defName;
  if (consumeIf(FIRToken::kw_defname)) {
    if (parseToken(FIRToken::equal, "expected '=' in defname") ||
        parseId(defName, "expected defname name"))
      return failure();
  }

  NamedAttrList parameters;
  SmallPtrSet<Identifier, 8> seenNames;

  // Parse the parameter list.
  while (consumeIf(FIRToken::kw_parameter)) {
    auto loc = getToken().getLoc();
    StringRef paramName;
    if (parseId(paramName, "expected parameter name") ||
        parseToken(FIRToken::equal, "expected '=' in parameter"))
      return failure();

    Attribute value;
    switch (getToken().getKind()) {
    default:
      return emitError("expected parameter value"), failure();

    case FIRToken::integer:
    case FIRToken::signed_integer: {
      APInt result;
      if (parseIntLit(result, "invalid integer parameter"))
        return failure();

      value = builder.getIntegerAttr(
          builder.getIntegerType(result.getBitWidth()), result);
      break;
    }
    case FIRToken::string: {
      // Drop the double quotes and unescape.
      value = builder.getStringAttr(getToken().getStringValue());
      consumeToken(FIRToken::string);
      break;
    }
    case FIRToken::raw_string: {
      // Drop the single quotes and unescape the ones inside.
      value = builder.getStringAttr(getToken().getRawStringValue());
      consumeToken(FIRToken::raw_string);
      break;
    }

    case FIRToken::floatingpoint:
      double v;
      if (!llvm::to_float(getTokenSpelling(), v))
        return emitError("invalid float parameter syntax"), failure();

      value = builder.getF64FloatAttr(v);
      consumeToken(FIRToken::floatingpoint);
      break;
    }

    auto nameId = builder.getIdentifier(paramName);
    if (!seenNames.insert(nameId).second)
      return emitError(loc, "redefinition of parameter '" + paramName + "'");
    parameters.append(nameId, value);
  }

  auto fmodule = builder.create<FExtModuleOp>(info.getLoc(), name, portList,
                                              defName, annotations);

  if (!parameters.empty())
    fmodule->setAttr("parameters",
                     DictionaryAttr::get(getContext(), parameters));

  return success();
}

// Parse the body of this module.
ParseResult
FIRCircuitParser::parseModuleBody(DeferredModuleToParse &deferredModule) {
  FModuleOp moduleOp = deferredModule.moduleOp;
  auto &portLocs = deferredModule.portLocs;
  auto &moduleTarget = deferredModule.moduleTarget;

  // We parse the body of this module with its own lexer, enabling parallel
  // parsing with the rest of the other module bodies.
  FIRLexer moduleBodyLexer(getLexer().getSourceMgr(), getContext());

  // Reset the parser/lexer state back to right after the port list.
  deferredModule.lexerCursor.restore(moduleBodyLexer);

  FIRModuleContext moduleContext(getConstants(), moduleBodyLexer,
                                 std::move(moduleTarget));

  // Install all of the ports into the symbol table, associated with their
  // block arguments.
  auto argIt = moduleOp.args_begin();
  auto portList = getModulePortInfo(moduleOp);
  for (auto portAndLoc : llvm::zip(portList, portLocs)) {
    ModulePortInfo &port = std::get<0>(portAndLoc);
    if (moduleContext.addSymbolEntry(port.getName(), *argIt,
                                     std::get<1>(portAndLoc)))
      return failure();
    ++argIt;
  }

  FIRStmtParser stmtParser(*moduleOp.getBodyBlock(), moduleContext);

  // Parse the moduleBlock.
  auto result = stmtParser.parseSimpleStmtBlock(deferredModule.indent);
  deferredModule.targetSet = std::move(moduleContext.targetsInModule);
  return result;
}

/// file ::= circuit
/// circuit ::= 'circuit' id ':' info? INDENT module* DEDENT EOF
///
/// If non-null, annotationsBuf is a memory buffer containing JSON annotations.
///
ParseResult
FIRCircuitParser::parseCircuit(const llvm::MemoryBuffer *annotationsBuf) {
  auto indent = getIndentation();
  if (!indent.hasValue())
    return emitError("'circuit' must be first token on its line"), failure();
  unsigned circuitIndent = indent.getValue();

  LocWithInfo info(getToken().getLoc(), this);
  StringAttr name;
  SMLoc inlineAnnotationsLoc;
  StringRef inlineAnnotations;

  // A file must contain a top level `circuit` definition.
  if (parseToken(FIRToken::kw_circuit,
                 "expected a top-level 'circuit' definition") ||
      parseId(name, "expected circuit name") ||
      parseToken(FIRToken::colon, "expected ':' in circuit definition") ||
      parseOptionalAnnotations(inlineAnnotationsLoc, inlineAnnotations) ||
      info.parseOptionalInfo())
    return failure();

  std::string circuitTarget = "~" + name.getValue().str();

  // Deal with any inline annotations, if they exist.  These are processed first
  // to place any annotations from an annotation file *after* the inline
  // annotations.  While arbitrary, this makes the annotation file have "append"
  // semantics.
  if (!inlineAnnotations.empty())
    if (importAnnotations(inlineAnnotationsLoc, circuitTarget,
                          inlineAnnotations))
      return failure();

  // Deal with the annotation file if one was specified
  if (annotationsBuf) {
    if (importAnnotations(info.getFIRLoc(), circuitTarget,
                          annotationsBuf->getBuffer()))
      return failure();
  }

  OpBuilder b(mlirModule.getBodyRegion());

  // Get annotations associated with this circuit. These are either:
  //   1. Annotations with no target (which we use "~" to identify)
  //   2. Annotations targeting the circuit, e.g., "~Foo"
  ArrayAttr annotations = getAnnotations({"~", circuitTarget}, info.getFIRLoc(),
                                         getConstants().targetSet);

  // Create the top-level circuit op in the MLIR module.
  auto circuit = b.create<CircuitOp>(info.getLoc(), name, annotations);
  deferredModules.reserve(16);

  // Parse any contained modules.
  while (true) {
    switch (getToken().getKind()) {
    // If we got to the end of the file, then we're done.
    case FIRToken::eof:
      goto DoneParsing;

    // If we got an error token, then the lexer already emitted an error,
    // just stop.  We could introduce error recovery if there was demand for
    // it.
    case FIRToken::error:
      return failure();

    default:
      emitError("unexpected token in circuit");
      return failure();

    case FIRToken::kw_module:
    case FIRToken::kw_extmodule: {
      auto indent = getIndentation();
      if (!indent.hasValue())
        return emitError("'module' must be first token on its line"), failure();
      unsigned moduleIndent = indent.getValue();

      if (moduleIndent <= circuitIndent)
        return emitError("module should be indented more"), failure();

      if (parseModule(circuit, circuitTarget, moduleIndent))
        return failure();
      break;
    }
    }
  }

DoneParsing:

  auto anyFailed = mlir::failableParallelForEachN(
      getContext(), 0, deferredModules.size(), [&](size_t index) {
        if (parseModuleBody(deferredModules[index]))
          return failure();
        return success();
      });
  if (failed(anyFailed))
    return failure();

  // Mutate the global targetSet by taking the union of it and all targetSets
  // that were built up when modules were parsed.
  for (DeferredModuleToParse &deferredModule : deferredModules)
    getConstants().targetSet.insert(deferredModule.targetSet.begin(),
                                    deferredModule.targetSet.end());

  // Error if any Annotations were not applied to locations in the IR.
  bool foundUnappliedAnnotations = false;
  for (auto &entry : getConstants().annotationMap) {
    if (getConstants().targetSet.contains(entry.getKey()))
      continue;

    foundUnappliedAnnotations = true;
    mlir::emitError(circuit.getLoc())
        << "unapplied annotations with target '" << entry.getKey()
        << "' and payload '" << entry.getValue() << "'\n";
  }

  if (foundUnappliedAnnotations)
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

// Parse the specified .fir file into the specified MLIR context.
OwningModuleRef circt::firrtl::importFIRRTL(SourceMgr &sourceMgr,
                                            MLIRContext *context,
                                            FIRParserOptions options) {
  auto sourceBuf = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID());
  const llvm::MemoryBuffer *annotationsBuf = nullptr;
  if (sourceMgr.getNumBuffers() > 1)
    annotationsBuf = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID() + 1);

  context->loadDialect<FIRRTLDialect>();

  // This is the result module we are parsing into.
  OwningModuleRef module(ModuleOp::create(
      FileLineColLoc::get(context, sourceBuf->getBufferIdentifier(), /*line=*/0,
                          /*column=*/0)));
  SharedParserConstants state(context, options);
  FIRLexer lexer(sourceMgr, context);
  if (FIRCircuitParser(state, lexer, *module).parseCircuit(annotationsBuf))
    return nullptr;

  // Make sure the parse module has no other structural problems detected by
  // the verifier.
  if (failed(verify(*module)))
    return {};

  return module;
}

void circt::firrtl::registerFromFIRRTLTranslation() {
  static mlir::TranslateToMLIRRegistration fromFIR(
      "import-firrtl", [](llvm::SourceMgr &sourceMgr, MLIRContext *context) {
        return importFIRRTL(sourceMgr, context);
      });
}

/// Parse a string as a UInt/SInt literal.
FailureOr<Value> circt::firrtl::parseIntegerLiteralExp(Block *into,
                                                       StringRef input,
                                                       Location loc) {
  // Populate a new source manager with the string input.
  SourceMgr sourceMgr;
  sourceMgr.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(input),
                               SMLoc());

  // Setup a FIR parser.
  FIRParserOptions options;
  SharedParserConstants state(into->getParent()->getContext(), options);
  FIRLexer lexer(sourceMgr, into->getParent()->getContext());
  FIRModuleContext moduleContext(state, lexer, "");
  FIRStmtParser parser(*into, moduleContext);

  // Parse the literal.
  Value value;
  parser.locationProcessor.setInfoLoc(loc);
  parser.locationProcessor.startStatement();
  auto result = parser.parseIntegerLiteralExp(value);
  parser.locationProcessor.endStatement(parser);

  if (result)
    return failure();
  return value;
}
