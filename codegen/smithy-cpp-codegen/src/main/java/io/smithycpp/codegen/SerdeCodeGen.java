package io.smithycpp.codegen;

import java.util.Optional;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.traits.TimestampFormatTrait;

/**
 * Member-level serde emission shared by the serde file generator and both halves of the protocol
 * generators: the wire-name policy, C++ expressions that turn a typed value into a Document node,
 * statements that read one back into a typed lvalue, and the one member-serialize / member-parse
 * skeletons everything rides so the shared branches cannot drift (issue #72).
 */
final class SerdeCodeGen {

  private final CppContext context;

  /** Whether the module's protocol honors @jsonName (ProtocolGenerator.usesJsonName). */
  private final boolean useJsonName;

  SerdeCodeGen(CppContext context, boolean useJsonName) {
    this.context = context;
    this.useJsonName = useJsonName;
  }

  /**
   * The JSON document key for a member. Riding on this instance keeps the serde functions and the
   * binding emitters (which all take a SerdeCodeGen) on one policy by construction.
   */
  String wireName(MemberShape member) {
    return HttpBindingCodeGen.wireName(member, useJsonName);
  }

  /**
   * Writes one member of {@code owner} into an in-scope DocumentMap named {@code mapVar} (unset
   * optionals are skipped). The one member-serialize skeleton — serde's whole-structure serializer
   * and the HTTP binding body writer both ride it, so they cannot drift (issue #72).
   */
  void writeMemberSerialize(CppWriter w, MemberShape member, String owner, String mapVar) {
    String field = owner + "." + context.cppSymbols().toMemberName(member);
    // Populated @default members are plain and always serialize their value.
    if (MemberDefaults.plain(context.model(), member)) {
      w.write("$L.emplace($S, $L);", mapVar, wireName(member), serializeExpression(member, field));
    } else {
      w.openBlock("if ($L.has_value()) {", field);
      w.write(
          "$L.emplace($S, $L);",
          mapVar,
          wireName(member),
          serializeExpression(member, "(*" + field + ")"));
      w.closeBlock("}");
    }
  }

  /**
   * The C++ condition for "the required member was absent or null" — written against the {@code
   * member} local {@link #writeMemberRead} declares, so hook implementations use this constant
   * instead of hand-writing a condition coupled to that variable name.
   */
  static final String MEMBER_ABSENT = "member == nullptr || member->is_null()";

  /** Emits the required-member-absent branch of {@link #writeMemberRead}. */
  interface RequiredAbsentEmitter {
    /**
     * {@code member} was required but the document carried null or nothing; {@code
     * deserializeMember} emits the member's deserialization for the branch where it was present.
     */
    void write(CppWriter w, MemberShape member, Runnable deserializeMember);
  }

  /**
   * Reads one member out of a decoded document map into {@code targetPrefix}<member> — the one
   * member-parse skeleton (issue #72). The lenient-required (@required + @default keeps the
   * initializer) and optional branches are identical for serde functions and both HTTP wire ends by
   * construction; what a missing required member means is the caller's {@code requiredAbsent} hook
   * (serde and clients fail the parse, servers record a validation failure), and only serde
   * fills @input defaults inline ({@code fillDefaults} — the HTTP server fills them post-parse
   * across all binding locations). {@code mapAccess} reaches the decoded document: {@code "doc."}
   * for serde's by-value parameter, {@code "body_doc->"} for the binding readers' Outcome.
   */
  void writeMemberRead(
      CppWriter w,
      MemberShape member,
      String mapAccess,
      String targetPrefix,
      String structType,
      boolean fillDefaults,
      RequiredAbsentEmitter requiredAbsent) {
    String field = targetPrefix + context.cppSymbols().toMemberName(member);
    String path = structType + "." + member.getMemberName();
    w.openBlock("{");
    w.write("const smithy::Document* member = $LFind($S);", mapAccess, wireName(member));
    if (MemberDefaults.lenientRequired(context.model(), member)) {
      // @required + @default (the evolution pattern): absence keeps the
      // member's default initializer instead of failing.
      w.openBlock("if (member != nullptr && !member->is_null()) {");
      writeDeserializeInto(w, member, "member", field, path);
      w.closeBlock("}");
    } else if (member.isRequired()) {
      requiredAbsent.write(w, member, () -> writeDeserializeInto(w, member, "member", field, path));
    } else {
      w.openBlock("if (member != nullptr && !member->is_null()) {");
      w.write("$L parsed_member{};", context.cppSymbols().typeRef(target(member)));
      writeDeserializeInto(w, member, "member", "parsed_member", path);
      w.write("$L = std::move(parsed_member);", field);
      if (fillDefaults && MemberDefaults.fillOnParse(context.model(), member)) {
        // @input members stay client-optional, but servers (the only
        // consumers of input deserializers) fill the default when absent.
        w.closeBlock("} else {");
        w.indent();
        w.write("$L = $L;", field, MemberDefaults.literal(context, member));
        w.dedent();
        w.write("}");
      } else {
        w.closeBlock("}");
      }
    }
    w.closeBlock("}");
  }

  private Shape target(MemberShape member) {
    return context.model().expectShape(member.getTarget());
  }

  static String serdeFunctionSuffix(CppContext context, Shape shape) {
    return context.cppSymbols().declaredName(shape);
  }

  /** C++ TimestampFormat constant for a member, honoring @timestampFormat. */
  String timestampFormat(MemberShape member) {
    return timestampFormat(member, "smithy::TimestampFormat::kEpochSeconds");
  }

  String timestampFormat(MemberShape member, String defaultConstant) {
    Optional<TimestampFormatTrait> trait =
        member
            .getTrait(TimestampFormatTrait.class)
            .or(() -> target(member).getTrait(TimestampFormatTrait.class));
    if (trait.isEmpty()) {
      return defaultConstant;
    }
    return switch (trait.get().getValue()) {
      case "date-time" -> "smithy::TimestampFormat::kDateTime";
      case "http-date" -> "smithy::TimestampFormat::kHttpDate";
      default -> "smithy::TimestampFormat::kEpochSeconds";
    };
  }

  /** Expression producing a smithy::Document from {@code valueExpr} of the member's type. */
  String serializeExpression(MemberShape member, String valueExpr) {
    Shape shape = target(member);
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      return "smithy::Document(smithy::DocumentMap{})";
    }
    return switch (shape.getType()) {
      case BOOLEAN -> "smithy::Document(" + valueExpr + ")";
      case BYTE, SHORT, INTEGER, LONG ->
          "smithy::Document(static_cast<std::int64_t>(" + valueExpr + "))";
      case INT_ENUM -> "smithy::Document(static_cast<std::int64_t>(" + valueExpr + "))";
      case FLOAT, DOUBLE -> "smithy::Document(static_cast<double>(" + valueExpr + "))";
      case STRING -> "smithy::Document(" + valueExpr + ")";
      case ENUM -> "smithy::Document(std::string(" + valueExpr + ".ToString()))";
      case BLOB -> "smithy::Document(" + valueExpr + ")";
      case TIMESTAMP ->
          "smithy::Document::FromTimestamp(" + valueExpr + ", " + timestampFormat(member) + ")";
      case DOCUMENT -> valueExpr;
      // Boxed (recursive) members dereference through the smithy::Boxed.
      case STRUCTURE, UNION, LIST, MAP ->
          context.cppSymbols().recursion().isBoxed(member)
              ? "Serialize" + serdeFunctionSuffix(context, shape) + "(*(" + valueExpr + "))"
              : "Serialize" + serdeFunctionSuffix(context, shape) + "(" + valueExpr + ")";
      default ->
          throw new CodegenException(
              "cpp-codegen: cannot serialize member targeting " + shape.getId());
    };
  }

  /**
   * Emits statements reading {@code *docExpr} (a non-null, non-null-node Document pointer) into
   * {@code outExpr}, returning a serialization Error on mismatch. {@code path} names the member in
   * error messages.
   */
  void writeDeserializeInto(
      CppWriter w, MemberShape member, String docExpr, String outExpr, String path) {
    Shape shape = target(member);
    String wrong =
        "return smithy::Error::Serialization(\"" + path + ": unexpected type on the wire\");";
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      w.write("$L = smithy::Unit{};", outExpr);
      return;
    }
    switch (shape.getType()) {
      case BOOLEAN -> {
        w.write("if (!$L->is_bool()) $L", docExpr, wrong);
        w.write("$L = $L->as_bool();", outExpr, docExpr);
      }
      case BYTE, SHORT, INTEGER, LONG, INT_ENUM -> {
        String type = context.cppSymbols().typeRef(shape);
        w.write("if (!$L->is_int()) $L", docExpr, wrong);
        if (shape.getType() != software.amazon.smithy.model.shapes.ShapeType.LONG) {
          // Narrower integers — intEnum included, its underlying type is
          // int32 — reject out-of-range wire values instead of truncating
          // (the malformed-request suite pins this).
          String bounds =
              switch (shape.getType()) {
                case BYTE -> "-128 || " + docExpr + "->as_int() > 127";
                case SHORT -> "-32768 || " + docExpr + "->as_int() > 32767";
                default -> "-2147483648LL || " + docExpr + "->as_int() > 2147483647LL";
              };
          w.write(
              "if ($L->as_int() < $L) return smithy::Error::Serialization(\"$L: value out of "
                  + "range\");",
              docExpr,
              bounds,
              path);
        }
        w.write("$L = static_cast<$L>($L->as_int());", outExpr, type, docExpr);
      }
      case FLOAT -> {
        // The double→float narrowing is checked: a finite wire value beyond
        // float range would be UB to cast (UBSan float-cast-overflow).
        w.openBlock("{");
        w.write("auto parsed = smithy::DoubleFromDocument(*$L);", docExpr);
        w.write(
            "if (!parsed) return smithy::Error::Serialization($S);", path + ": expected a number");
        w.write("auto narrowed = smithy::FloatFromDouble(*parsed);");
        w.write(
            "if (!narrowed) return smithy::Error::Serialization($S);",
            path + ": value out of range");
        w.write("$L = *narrowed;", outExpr);
        w.closeBlock("}");
      }
      case DOUBLE -> {
        String type = context.cppSymbols().typeRef(shape);
        w.openBlock("{");
        w.write("auto parsed = smithy::DoubleFromDocument(*$L);", docExpr);
        w.write(
            "if (!parsed) return smithy::Error::Serialization($S);", path + ": expected a number");
        w.write("$L = static_cast<$L>(*parsed);", outExpr, type);
        w.closeBlock("}");
      }
      case STRING -> {
        w.write("if (!$L->is_string()) $L", docExpr, wrong);
        w.write("$L = $L->as_string();", outExpr, docExpr);
      }
      case ENUM -> {
        String type = context.cppSymbols().typeRef(shape);
        w.write("if (!$L->is_string()) $L", docExpr, wrong);
        w.write("$L = $L::FromString($L->as_string());", outExpr, type, docExpr);
      }
      case BLOB -> {
        w.openBlock("{");
        w.write("auto parsed = smithy::BlobFromDocument(*$L);", docExpr);
        w.write("if (!parsed) return std::move(parsed).error();");
        w.write("$L = std::move(*parsed);", outExpr);
        w.closeBlock("}");
      }
      case TIMESTAMP -> {
        w.openBlock("{");
        w.write(
            "auto parsed = smithy::TimestampFromDocument(*$L, $L);",
            docExpr,
            timestampFormat(member));
        w.write("if (!parsed) return std::move(parsed).error();");
        w.write("$L = *parsed;", outExpr);
        w.closeBlock("}");
      }
      case DOCUMENT -> w.write("$L = *$L;", outExpr, docExpr);
      case STRUCTURE, UNION, LIST, MAP -> {
        w.openBlock("{");
        w.write("auto parsed = Deserialize$L(*$L);", serdeFunctionSuffix(context, shape), docExpr);
        w.write("if (!parsed) return std::move(parsed).error();");
        w.write("$L = std::move(*parsed);", outExpr);
        w.closeBlock("}");
      }
      default ->
          throw new CodegenException(
              "cpp-codegen: cannot deserialize member targeting " + shape.getId());
    }
  }
}
