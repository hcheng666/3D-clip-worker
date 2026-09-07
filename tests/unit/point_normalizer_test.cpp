#include "clip_worker/normalization/point_normalizer.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/point_canonical_writer.hpp"
#include "support/synthetic_pnts.hpp"

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

TEST(PointNormalizerTest, DecodesQuantizationRtcColorNormalFeatureAndProperties) {
    const auto fixture = tests::makeQuantizedPntsFixture();
    const auto scene = PointNormalizer().normalize(formats::ByteView(fixture.bytes));

    ASSERT_EQ(scene.pointCount(), 2U);
    EXPECT_FLOAT_EQ(scene.positions[0U], 10.0F);
    EXPECT_FLOAT_EQ(scene.positions[1U], 30.0F);
    EXPECT_FLOAT_EQ(scene.positions[2U], -20.0F);
    EXPECT_EQ(scene.colors_rgba,
              (std::vector<std::uint8_t>{255U, 0U, 0U, 255U,
                                         0U, 255U, 0U, 255U}));
    EXPECT_EQ(scene.feature_ids, (std::vector<std::uint32_t>{0U, 1U}));
    ASSERT_TRUE(scene.legacy_properties.has_value());
    EXPECT_EQ(scene.legacy_properties->feature_count, 2U);

    const auto first = PointCanonicalWriter::write(scene);
    const auto second = PointCanonicalWriter::write(scene);
    EXPECT_EQ(first.glb, second.glb);
    EXPECT_EQ(first.point_count, 2U);
    const ToolVersion validator{"POINT_CANONICAL_VALIDATOR", "1.0.0",
                                std::string(64U, 'a')};
    const auto evidence = validateCanonicalGlb(
            first.glb, CanonicalFamily::point_gltf2, validator);
    EXPECT_EQ(evidence.validation_summary.feature_count, 2U);
    EXPECT_EQ(evidence.validation_summary.feature_identity_model,
              FeatureIdentityModel::point_feature_id);
}

TEST(PointNormalizerTest, EnforcesNamedPointLimitBeforeDecode) {
    const auto fixture = tests::makeQuantizedPntsFixture();
    PointResourceLimits limits;
    limits.maximum_points = 1U;
    EXPECT_THROW(PointNormalizer(limits).normalize(
                         formats::ByteView(fixture.bytes)),
                 formats::FormatError);
}

TEST(PointNormalizerTest, UsesPointOrdinalsForPerPointProperties) {
    const auto fixture = tests::makePerPointPropertyPntsFixture();
    const auto scene = PointNormalizer().normalize(
            formats::ByteView(fixture.bytes));

    EXPECT_EQ(scene.feature_ids,
              (std::vector<std::uint32_t>{0U, 1U}));
    ASSERT_TRUE(scene.legacy_properties.has_value());
    EXPECT_EQ(scene.legacy_properties->feature_count, 2U);
    ASSERT_EQ(scene.legacy_properties->columns.size(), 2U);
}

}  // namespace
}  // namespace clip_worker::normalization
