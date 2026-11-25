import SwiftUI
import UIKit
import SceneKit
import GameController

// MARK: - Color Palettes
public struct MiiColors {
    public static let favoriteColors: [(name: String, color: Color, uiColor: UIColor)] = [
        ("Red", Color(red: 0.85, green: 0.16, blue: 0.12), UIColor(red: 0.85, green: 0.16, blue: 0.12, alpha: 1.0)),
        ("Orange", Color(red: 0.96, green: 0.49, blue: 0.12), UIColor(red: 0.96, green: 0.49, blue: 0.12, alpha: 1.0)),
        ("Yellow", Color(red: 0.98, green: 0.87, blue: 0.12), UIColor(red: 0.98, green: 0.87, blue: 0.12, alpha: 1.0)),
        ("Lime", Color(red: 0.54, green: 0.83, blue: 0.13), UIColor(red: 0.54, green: 0.83, blue: 0.13, alpha: 1.0)),
        ("Green", Color(red: 0.05, green: 0.52, blue: 0.22), UIColor(red: 0.05, green: 0.52, blue: 0.22, alpha: 1.0)),
        ("Blue", Color(red: 0.00, green: 0.30, blue: 0.64), UIColor(red: 0.00, green: 0.30, blue: 0.64, alpha: 1.0)),
        ("Cyan", Color(red: 0.14, green: 0.65, blue: 0.87), UIColor(red: 0.14, green: 0.65, blue: 0.87, alpha: 1.0)),
        ("Pink", Color(red: 0.91, green: 0.32, blue: 0.52), UIColor(red: 0.91, green: 0.32, blue: 0.52, alpha: 1.0)),
        ("Purple", Color(red: 0.46, green: 0.15, blue: 0.52), UIColor(red: 0.46, green: 0.15, blue: 0.52, alpha: 1.0)),
        ("Brown", Color(red: 0.41, green: 0.22, blue: 0.07), UIColor(red: 0.41, green: 0.22, blue: 0.07, alpha: 1.0)),
        ("White", Color(red: 0.95, green: 0.95, blue: 0.95), UIColor(red: 0.95, green: 0.95, blue: 0.95, alpha: 1.0)),
        ("Black", Color(red: 0.15, green: 0.15, blue: 0.15), UIColor(red: 0.15, green: 0.15, blue: 0.15, alpha: 1.0))
    ]

    public static let skinTones: [(name: String, color: Color, uiColor: UIColor)] = [
        ("Tone 1", Color(red: 0.99, green: 0.89, blue: 0.82), UIColor(red: 0.99, green: 0.89, blue: 0.82, alpha: 1.0)),
        ("Tone 2", Color(red: 0.97, green: 0.82, blue: 0.73), UIColor(red: 0.97, green: 0.82, blue: 0.73, alpha: 1.0)),
        ("Tone 3", Color(red: 0.95, green: 0.75, blue: 0.64), UIColor(red: 0.95, green: 0.75, blue: 0.64, alpha: 1.0)),
        ("Tone 4", Color(red: 0.91, green: 0.65, blue: 0.51), UIColor(red: 0.91, green: 0.65, blue: 0.51, alpha: 1.0)),
        ("Tone 5", Color(red: 0.78, green: 0.53, blue: 0.38), UIColor(red: 0.78, green: 0.53, blue: 0.38, alpha: 1.0)),
        ("Tone 6", Color(red: 0.66, green: 0.40, blue: 0.25), UIColor(red: 0.66, green: 0.40, blue: 0.25, alpha: 1.0)),
        ("Tone 7", Color(red: 0.54, green: 0.29, blue: 0.16), UIColor(red: 0.54, green: 0.29, blue: 0.16, alpha: 1.0)),
        ("Tone 8", Color(red: 0.42, green: 0.20, blue: 0.08), UIColor(red: 0.42, green: 0.20, blue: 0.08, alpha: 1.0)),
        ("Tone 9", Color(red: 0.29, green: 0.13, blue: 0.05), UIColor(red: 0.29, green: 0.13, blue: 0.05, alpha: 1.0)),
        ("Tone 10", Color(red: 0.20, green: 0.07, blue: 0.02), UIColor(red: 0.20, green: 0.07, blue: 0.02, alpha: 1.0))
    ]

    public static let hairColors: [(name: String, color: Color, uiColor: UIColor)] = [
        ("Black", Color(red: 0.10, green: 0.10, blue: 0.10), UIColor(red: 0.10, green: 0.10, blue: 0.10, alpha: 1.0)),
        ("Dark Brown", Color(red: 0.26, green: 0.17, blue: 0.11), UIColor(red: 0.26, green: 0.17, blue: 0.11, alpha: 1.0)),
        ("Brown", Color(red: 0.42, green: 0.26, blue: 0.15), UIColor(red: 0.42, green: 0.26, blue: 0.15, alpha: 1.0)),
        ("Light Brown", Color(red: 0.58, green: 0.38, blue: 0.22), UIColor(red: 0.58, green: 0.38, blue: 0.22, alpha: 1.0)),
        ("Platinum", Color(red: 0.88, green: 0.80, blue: 0.65), UIColor(red: 0.88, green: 0.80, blue: 0.65, alpha: 1.0)),
        ("Auburn", Color(red: 0.60, green: 0.22, blue: 0.12), UIColor(red: 0.60, green: 0.22, blue: 0.12, alpha: 1.0)),
        ("Gray", Color(red: 0.55, green: 0.55, blue: 0.55), UIColor(red: 0.55, green: 0.55, blue: 0.55, alpha: 1.0)),
        ("White", Color(red: 0.90, green: 0.90, blue: 0.90), UIColor(red: 0.90, green: 0.90, blue: 0.90, alpha: 1.0))
    ]

    public static let eyeColors: [(name: String, color: Color, uiColor: UIColor)] = [
        ("Black", Color(red: 0.12, green: 0.12, blue: 0.12), UIColor(red: 0.12, green: 0.12, blue: 0.12, alpha: 1.0)),
        ("Brown", Color(red: 0.38, green: 0.23, blue: 0.12), UIColor(red: 0.38, green: 0.23, blue: 0.12, alpha: 1.0)),
        ("Red", Color(red: 0.70, green: 0.15, blue: 0.15), UIColor(red: 0.70, green: 0.15, blue: 0.15, alpha: 1.0)),
        ("Blue", Color(red: 0.15, green: 0.45, blue: 0.75), UIColor(red: 0.15, green: 0.45, blue: 0.75, alpha: 1.0)),
        ("Green", Color(red: 0.20, green: 0.60, blue: 0.25), UIColor(red: 0.20, green: 0.60, blue: 0.25, alpha: 1.0)),
        ("Hazel", Color(red: 0.65, green: 0.45, blue: 0.15), UIColor(red: 0.65, green: 0.45, blue: 0.15, alpha: 1.0))
    ]

    public static let glassesColors: [(name: String, color: Color, uiColor: UIColor)] = [
        ("Gray", Color(red: 0.3, green: 0.3, blue: 0.3), UIColor(red: 0.3, green: 0.3, blue: 0.3, alpha: 1.0)),
        ("Brown", Color(red: 0.45, green: 0.25, blue: 0.15), UIColor(red: 0.45, green: 0.25, blue: 0.15, alpha: 1.0)),
        ("Red", Color(red: 0.8, green: 0.2, blue: 0.2), UIColor(red: 0.8, green: 0.2, blue: 0.2, alpha: 1.0)),
        ("Blue", Color(red: 0.2, green: 0.4, blue: 0.8), UIColor(red: 0.2, green: 0.4, blue: 0.8, alpha: 1.0)),
        ("Yellow", Color(red: 0.9, green: 0.7, blue: 0.1), UIColor(red: 0.9, green: 0.7, blue: 0.1, alpha: 1.0)),
        ("White", Color(red: 0.9, green: 0.9, blue: 0.9), UIColor(red: 0.9, green: 0.9, blue: 0.9, alpha: 1.0))
    ]
}

// MARK: - Mii Icon Provider (Firmware 2D Assets)
public class MiiIconProvider {
    public static let shared = MiiIconProvider()
    private var cache = [String: UIImage]()

    private var iconDirectory: URL? {
        let bundle = Bundle.main
        if let url = bundle.url(forResource: "dm/mii_icons", withExtension: nil) { return url }
        if let url = bundle.url(forResource: "mii_icons", withExtension: nil) { return url }
        if let url = bundle.url(forResource: "dm/system/mii_icons", withExtension: nil) { return url }
        // Fallback to Documents / App support if running from staged paths
        let home = URL(fileURLWithPath: NSHomeDirectory())
        let docIcons = home.appendingPathComponent("Documents/mii_icons")
        if FileManager.default.fileExists(atPath: docIcons.path) { return docIcons }
        return nil
    }

    public func icon(named name: String) -> UIImage? {
        if let cached = cache[name] { return cached }
        if let dir = iconDirectory {
            let path = dir.appendingPathComponent("\(name).png").path
            if let img = UIImage(contentsOfFile: path) {
                cache[name] = img
                return img
            }
        }
        return nil
    }
}

// MARK: - Mii Model Data
public class MiiModelData: ObservableObject {
    @Published public var nickname: String = "Player"
    @Published public var gender: Int = 0 // 0 = Male, 1 = Female
    @Published public var favoriteColor: Int = 0 // 0..11
    @Published public var height: Int = 64 // 0..128
    @Published public var build: Int = 64 // 0..128

    // Face
    @Published public var facelineType: Int = 0 // 0..11
    @Published public var facelineColor: Int = 0 // 0..9 (skin tone)
    @Published public var facelineWrinkle: Int = 0 // 0..11
    @Published public var facelineMake: Int = 0 // 0..11

    // Hair
    @Published public var hairType: Int = 0 // 0..131
    @Published public var hairColor: Int = 0 // 0..7
    @Published public var hairFlip: Int = 0 // 0 = Normal, 1 = Flipped

    // Eyebrows
    @Published public var eyebrowType: Int = 0 // 0..23
    @Published public var eyebrowColor: Int = 0
    @Published public var eyebrowScale: Int = 4 // 0..8
    @Published public var eyebrowAspect: Int = 3 // 0..6
    @Published public var eyebrowRotate: Int = 6 // 0..11
    @Published public var eyebrowX: Int = 6 // 0..12
    @Published public var eyebrowY: Int = 10 // 3..18

    // Eyes
    @Published public var eyeType: Int = 0 // 0..59
    @Published public var eyeColor: Int = 0 // 0..5
    @Published public var eyeScale: Int = 4 // 0..7
    @Published public var eyeAspect: Int = 3 // 0..6
    @Published public var eyeRotate: Int = 4 // 0..7
    @Published public var eyeX: Int = 6 // 0..12
    @Published public var eyeY: Int = 10 // 0..18

    // Nose
    @Published public var noseType: Int = 0 // 0..17
    @Published public var noseScale: Int = 4 // 0..8
    @Published public var noseY: Int = 9 // 0..18

    // Mouth
    @Published public var mouthType: Int = 0 // 0..35
    @Published public var mouthColor: Int = 0 // 0..4
    @Published public var mouthScale: Int = 4 // 0..8
    @Published public var mouthAspect: Int = 3 // 0..6
    @Published public var mouthY: Int = 11 // 0..18

    // Facial Hair
    @Published public var mustacheType: Int = 0 // 0..5
    @Published public var mustacheScale: Int = 4 // 0..8
    @Published public var mustacheY: Int = 8 // 0..16
    @Published public var beardType: Int = 0 // 0..5
    @Published public var beardColor: Int = 0 // 0..7

    // Glasses
    @Published public var glassType: Int = 0 // 0..19
    @Published public var glassColor: Int = 0 // 0..5
    @Published public var glassScale: Int = 4 // 0..7
    @Published public var glassY: Int = 10 // 0..20

    // Mole
    @Published public var moleType: Int = 0 // 0 = None, 1 = Present
    @Published public var moleScale: Int = 4 // 0..8
    @Published public var moleX: Int = 8 // 0..16
    @Published public var moleY: Int = 15 // 0..30

    public init() {}

    public func load(from rawPtr: UnsafeRawPointer) {
        let ptr = rawPtr.bindMemory(to: UInt8.self, capacity: 0x58)

        // Nickname at 0x10 (11 UTF-16 words)
        var utf16Chars = [UInt16]()
        for i in 0..<10 {
            var val: UInt16 = 0
            memcpy(&val, ptr + 0x10 + i * 2, 2)
            if val == 0 { break }
            utf16Chars.append(val)
        }
        if !utf16Chars.isEmpty, let str = String(utf16CodeUnits: utf16Chars, count: utf16Chars.count) as String? {
            self.nickname = str
        } else {
            self.nickname = "Player"
        }

        self.favoriteColor = Int(ptr[0x27]) % 12
        self.gender = Int(ptr[0x28]) % 2
        self.height = Int(ptr[0x29])
        self.build = Int(ptr[0x2a])

        self.facelineType = Int(ptr[0x2d]) % 12
        self.facelineColor = Int(ptr[0x2e]) % 10
        self.facelineWrinkle = Int(ptr[0x2f]) % 12
        self.facelineMake = Int(ptr[0x30]) % 12

        self.hairType = Int(ptr[0x31]) % 132
        self.hairColor = Int(ptr[0x32]) % 8
        self.hairFlip = Int(ptr[0x33]) % 2

        self.eyeType = Int(ptr[0x34]) % 60
        self.eyeColor = Int(ptr[0x35]) % 6
        self.eyeScale = Int(ptr[0x36]) % 8
        self.eyeAspect = Int(ptr[0x37]) % 7
        self.eyeRotate = Int(ptr[0x38]) % 8
        self.eyeX = Int(ptr[0x39]) % 13
        self.eyeY = Int(ptr[0x3a]) % 19

        self.eyebrowType = Int(ptr[0x3b]) % 24
        self.eyebrowColor = Int(ptr[0x3c]) % 8
        self.eyebrowScale = Int(ptr[0x3d]) % 9
        self.eyebrowAspect = Int(ptr[0x3e]) % 7
        self.eyebrowRotate = Int(ptr[0x3f]) % 12
        self.eyebrowX = Int(ptr[0x40]) % 13
        self.eyebrowY = max(3, Int(ptr[0x41]) % 19)

        self.noseType = Int(ptr[0x42]) % 18
        self.noseScale = Int(ptr[0x43]) % 9
        self.noseY = Int(ptr[0x44]) % 19

        self.mouthType = Int(ptr[0x45]) % 36
        self.mouthColor = Int(ptr[0x46]) % 5
        self.mouthScale = Int(ptr[0x47]) % 9
        self.mouthAspect = Int(ptr[0x48]) % 7
        self.mouthY = Int(ptr[0x49]) % 19

        self.beardColor = Int(ptr[0x4a]) % 8
        self.beardType = Int(ptr[0x4b]) % 6
        self.mustacheType = Int(ptr[0x4c]) % 6
        self.mustacheScale = Int(ptr[0x4d]) % 9
        self.mustacheY = Int(ptr[0x4e]) % 17

        self.glassType = Int(ptr[0x4f]) % 20
        self.glassColor = Int(ptr[0x50]) % 6
        self.glassScale = Int(ptr[0x51]) % 8
        self.glassY = Int(ptr[0x52]) % 21

        self.moleType = Int(ptr[0x53]) % 2
        self.moleScale = Int(ptr[0x54]) % 9
        self.moleX = Int(ptr[0x55]) % 17
        self.moleY = Int(ptr[0x56]) % 31
    }

    public func save(to rawPtr: UnsafeMutableRawPointer) {
        let ptr = rawPtr.bindMemory(to: UInt8.self, capacity: 0x58)

        // Nickname
        let trimmed = String(nickname.prefix(10))
        var utf16 = Array(trimmed.utf16)
        while utf16.count < 11 { utf16.append(0) }
        for i in 0..<11 {
            var val = utf16[i]
            memcpy(ptr + 0x10 + i * 2, &val, 2)
        }

        ptr[0x27] = UInt8(favoriteColor)
        ptr[0x28] = UInt8(gender)
        ptr[0x29] = UInt8(height)
        ptr[0x2a] = UInt8(build)

        ptr[0x2d] = UInt8(facelineType)
        ptr[0x2e] = UInt8(facelineColor)
        ptr[0x2f] = UInt8(facelineWrinkle)
        ptr[0x30] = UInt8(facelineMake)

        ptr[0x31] = UInt8(hairType)
        ptr[0x32] = UInt8(hairColor)
        ptr[0x33] = UInt8(hairFlip)

        ptr[0x34] = UInt8(eyeType)
        ptr[0x35] = UInt8(eyeColor)
        ptr[0x36] = UInt8(eyeScale)
        ptr[0x37] = UInt8(eyeAspect)
        ptr[0x38] = UInt8(eyeRotate)
        ptr[0x39] = UInt8(eyeX)
        ptr[0x3a] = UInt8(eyeY)

        ptr[0x3b] = UInt8(eyebrowType)
        ptr[0x3c] = UInt8(eyebrowColor)
        ptr[0x3d] = UInt8(eyebrowScale)
        ptr[0x3e] = UInt8(eyebrowAspect)
        ptr[0x3f] = UInt8(eyebrowRotate)
        ptr[0x40] = UInt8(eyebrowX)
        ptr[0x41] = UInt8(eyebrowY)

        ptr[0x42] = UInt8(noseType)
        ptr[0x43] = UInt8(noseScale)
        ptr[0x44] = UInt8(noseY)

        ptr[0x45] = UInt8(mouthType)
        ptr[0x46] = UInt8(mouthColor)
        ptr[0x47] = UInt8(mouthScale)
        ptr[0x48] = UInt8(mouthAspect)
        ptr[0x49] = UInt8(mouthY)

        ptr[0x4a] = UInt8(beardColor)
        ptr[0x4b] = UInt8(beardType)
        ptr[0x4c] = UInt8(mustacheType)
        ptr[0x4d] = UInt8(mustacheScale)
        ptr[0x4e] = UInt8(mustacheY)

        ptr[0x4f] = UInt8(glassType)
        ptr[0x50] = UInt8(glassColor)
        ptr[0x51] = UInt8(glassScale)
        ptr[0x52] = UInt8(glassY)

        ptr[0x53] = UInt8(moleType)
        ptr[0x54] = UInt8(moleScale)
        ptr[0x55] = UInt8(moleX)
        ptr[0x56] = UInt8(moleY)
    }
}

// MARK: - SceneKit 3D Viewport
public struct MiiSceneView: UIViewRepresentable {
    @ObservedObject var mii: MiiModelData
    @Binding var cameraRotation: Float

    public func makeUIView(context: Context) -> SCNView {
        let scnView = SCNView()
        scnView.backgroundColor = UIColor(white: 0.12, alpha: 1.0)
        scnView.allowsCameraControl = true
        scnView.autoenablesDefaultLighting = false

        let scene = SCNScene()
        scnView.scene = scene

        // Camera setup
        let cameraNode = SCNNode()
        cameraNode.camera = SCNCamera()
        cameraNode.camera?.fieldOfView = 38
        cameraNode.position = SCNVector3(x: 0, y: 1.4, z: 4.8)
        cameraNode.look(at: SCNVector3(0, 1.2, 0))
        scene.rootNode.addChildNode(cameraNode)

        // Lighting
        let ambient = SCNNode()
        ambient.light = SCNLight()
        ambient.light?.type = .ambient
        ambient.light?.color = UIColor(white: 0.65, alpha: 1.0)
        scene.rootNode.addChildNode(ambient)

        let directional = SCNNode()
        directional.light = SCNLight()
        directional.light?.type = .directional
        directional.light?.color = UIColor(white: 0.75, alpha: 1.0)
        directional.light?.castsShadow = true
        directional.position = SCNVector3(x: 2.0, y: 4.0, z: 3.5)
        directional.look(at: SCNVector3(0, 1.2, 0))
        scene.rootNode.addChildNode(directional)

        let characterNode = SCNNode()
        characterNode.name = "CharacterRoot"
        scene.rootNode.addChildNode(characterNode)

        updateCharacter(characterNode, mii: mii)
        return scnView
    }

    public func updateUIView(_ uiView: SCNView, context: Context) {
        guard let scene = uiView.scene,
              let characterNode = scene.rootNode.childNode(withName: "CharacterRoot", recursively: false) else { return }
        updateCharacter(characterNode, mii: mii)
        if cameraRotation != 0 {
            characterNode.eulerAngles.y = cameraRotation
        }
    }

    private func updateCharacter(_ root: SCNNode, mii: MiiModelData) {
        root.childNodes.forEach { $0.removeFromParentNode() }

        let skinColor = MiiColors.skinTones[min(mii.facelineColor, MiiColors.skinTones.count - 1)].uiColor
        let favColor = MiiColors.favoriteColors[min(mii.favoriteColor, MiiColors.favoriteColors.count - 1)].uiColor
        let hairCol = MiiColors.hairColors[min(mii.hairColor, MiiColors.hairColors.count - 1)].uiColor

        // Height & Build scaling
        let heightScale = Float(0.85 + Double(mii.height) / 128.0 * 0.3)
        let buildScale = Float(0.85 + Double(mii.build) / 128.0 * 0.3)

        // Body Node (Torso + Limbs)
        let bodyNode = SCNNode()
        bodyNode.name = "Body"
        bodyNode.scale = SCNVector3(buildScale, heightScale, buildScale)

        // Torso / Shirt
        let torsoGeo = SCNCylinder(radius: 0.38, height: 0.8)
        torsoGeo.firstMaterial?.diffuse.contents = favColor
        let torsoNode = SCNNode(geometry: torsoGeo)
        torsoNode.position = SCNVector3(0, 0.45, 0)
        bodyNode.addChildNode(torsoNode)

        // Pants / Legs
        let legsGeo = SCNCylinder(radius: 0.32, height: 0.5)
        legsGeo.firstMaterial?.diffuse.contents = UIColor(white: 0.15, alpha: 1.0)
        let legsNode = SCNNode(geometry: legsGeo)
        legsNode.position = SCNVector3(0, 0.0, 0)
        bodyNode.addChildNode(legsNode)

        root.addChildNode(bodyNode)

        // Head Node
        let headNode = SCNNode()
        headNode.name = "Head"
        headNode.position = SCNVector3(0, 0.95 * heightScale + 0.65, 0)

        // Head shape geometry (morphing by facelineType)
        let radius: CGFloat = 0.58
        let headGeo = SCNSphere(radius: radius)
        headGeo.segmentCount = 48
        headGeo.firstMaterial?.diffuse.contents = skinColor
        headGeo.firstMaterial?.roughness.contents = 0.6

        // Subtle shape tweak
        let aspectY: Float = mii.facelineType % 2 == 0 ? 1.0 : 0.94
        let aspectX: Float = mii.facelineType % 3 == 0 ? 1.05 : 0.98
        headNode.scale = SCNVector3(aspectX, aspectY, 1.0)

        let skullNode = SCNNode(geometry: headGeo)
        headNode.addChildNode(skullNode)

        // Hair
        if mii.hairType > 0 {
            let hairNode = SCNNode()
            let hairGeo = SCNSphere(radius: radius * 1.05)
            hairGeo.segmentCount = 36
            hairGeo.firstMaterial?.diffuse.contents = hairCol

            let hairMesh = SCNNode(geometry: hairGeo)
            hairMesh.position = SCNVector3(0, 0.12, -0.05)
            hairMesh.scale = SCNVector3(1.04, 0.95, 1.05)
            hairNode.addChildNode(hairMesh)

            if mii.hairFlip == 1 {
                hairNode.scale = SCNVector3(-1.0, 1.0, 1.0)
            }
            headNode.addChildNode(hairNode)
        }

        // Facial Feature Decals (Eyes, Eyebrows, Nose, Mouth)
        func addDecal(textureName: String, pos: SCNVector3, size: CGSize, tint: UIColor? = nil, rotateDeg: Float = 0) {
            let plane = SCNPlane(width: size.width, height: size.height)
            if let img = MiiIconProvider.shared.icon(named: textureName) {
                plane.firstMaterial?.diffuse.contents = img
            } else {
                plane.firstMaterial?.diffuse.contents = UIColor.white
            }
            if let tint = tint {
                plane.firstMaterial?.multiply.contents = tint
            }
            plane.firstMaterial?.isDoubleSided = true
            plane.firstMaterial?.readsFromDepthBuffer = true
            plane.firstMaterial?.writesToDepthBuffer = false

            let node = SCNNode(geometry: plane)
            node.position = pos
            if rotateDeg != 0 {
                node.eulerAngles.z = rotateDeg * .pi / 180.0
            }
            headNode.addChildNode(node)
        }

        // Eyes
        let eyeTex = String(format: "IconEye%02d", mii.eyeType)
        let eyeSpread = Float(0.14 + Double(mii.eyeX) * 0.015)
        let eyeY = Float(0.04 - Double(mii.eyeY - 10) * 0.02)
        let eyeScale = Float(0.18 + Double(mii.eyeScale) * 0.02)
        let eyeRot = Float(mii.eyeRotate - 4) * 6.0
        let eyeColor = MiiColors.eyeColors[min(mii.eyeColor, MiiColors.eyeColors.count - 1)].uiColor

        addDecal(textureName: eyeTex,
                 pos: SCNVector3(-eyeSpread, eyeY, 0.58),
                 size: CGSize(width: Double(eyeScale), height: Double(eyeScale) * 0.8),
                 tint: eyeColor,
                 rotateDeg: -eyeRot)
        addDecal(textureName: eyeTex,
                 pos: SCNVector3(eyeSpread, eyeY, 0.58),
                 size: CGSize(width: Double(eyeScale), height: Double(eyeScale) * 0.8),
                 tint: eyeColor,
                 rotateDeg: eyeRot)

        // Eyebrows
        let browTex = String(format: "IconEyebrow%02d", mii.eyebrowType)
        let browSpread = Float(0.12 + Double(mii.eyebrowX) * 0.015)
        let browY = eyeY + Float(0.10 + Double(mii.eyebrowY - 10) * 0.015)
        let browScale = Float(0.16 + Double(mii.eyebrowScale) * 0.02)
        let browRot = Float(mii.eyebrowRotate - 6) * 5.0
        let browColor = MiiColors.hairColors[min(mii.eyebrowColor, MiiColors.hairColors.count - 1)].uiColor

        addDecal(textureName: browTex,
                 pos: SCNVector3(-browSpread, browY, 0.58),
                 size: CGSize(width: Double(browScale), height: Double(browScale) * 0.5),
                 tint: browColor,
                 rotateDeg: -browRot)
        addDecal(textureName: browTex,
                 pos: SCNVector3(browSpread, browY, 0.58),
                 size: CGSize(width: Double(browScale), height: Double(browScale) * 0.5),
                 tint: browColor,
                 rotateDeg: browRot)

        // Nose
        let noseTex = String(format: "IconNose%02d", mii.noseType)
        let noseY = Float(-0.06 - Double(mii.noseY - 9) * 0.02)
        let noseScale = Float(0.16 + Double(mii.noseScale) * 0.02)
        addDecal(textureName: noseTex,
                 pos: SCNVector3(0, noseY, 0.59),
                 size: CGSize(width: Double(noseScale), height: Double(noseScale)))

        // Mouth
        let mouthTex = String(format: "IconMouth%02d", mii.mouthType)
        let mouthY = noseY - Float(0.12 + Double(mii.mouthY - 11) * 0.018)
        let mouthScale = Float(0.20 + Double(mii.mouthScale) * 0.025)
        addDecal(textureName: mouthTex,
                 pos: SCNVector3(0, mouthY, 0.58),
                 size: CGSize(width: Double(mouthScale), height: Double(mouthScale) * 0.6))

        // Mustache
        if mii.mustacheType > 0 {
            let musTex = String(format: "IconMustache%02d", mii.mustacheType)
            let musY = mouthY + Float(0.04 - Double(mii.mustacheY - 8) * 0.015)
            let musScale = Float(0.22 + Double(mii.mustacheScale) * 0.025)
            let beardColor = MiiColors.hairColors[min(mii.beardColor, MiiColors.hairColors.count - 1)].uiColor
            addDecal(textureName: musTex,
                     pos: SCNVector3(0, musY, 0.585),
                     size: CGSize(width: Double(musScale), height: Double(musScale) * 0.5),
                     tint: beardColor)
        }

        // Beard
        if mii.beardType > 0 {
            let beardTex = String(format: "IconBeard%02d", mii.beardType - 1)
            let beardColor = MiiColors.hairColors[min(mii.beardColor, MiiColors.hairColors.count - 1)].uiColor
            addDecal(textureName: beardTex,
                     pos: SCNVector3(0, -0.32, 0.56),
                     size: CGSize(width: 0.45, height: 0.35),
                     tint: beardColor)
        }

        // Glasses
        if mii.glassType > 0 {
            let glassTex = String(format: "IconGlass%02d", mii.glassType)
            let glassY = eyeY + Float(0.02 - Double(mii.glassY - 10) * 0.015)
            let glassScale = Float(0.48 + Double(mii.glassScale) * 0.03)
            let glassColor = MiiColors.glassesColors[min(mii.glassColor, MiiColors.glassesColors.count - 1)].uiColor
            addDecal(textureName: glassTex,
                     pos: SCNVector3(0, glassY, 0.61),
                     size: CGSize(width: Double(glassScale), height: Double(glassScale) * 0.4),
                     tint: glassColor)
        }

        // Mole
        if mii.moleType > 0 {
            let moleX = Float(-0.25 + Double(mii.moleX) * 0.03)
            let moleY = Float(-0.20 + Double(mii.moleY) * 0.015)
            let moleScale = Float(0.03 + Double(mii.moleScale) * 0.006)
            addDecal(textureName: "IconMole00",
                     pos: SCNVector3(moleX, moleY, 0.59),
                     size: CGSize(width: Double(moleScale), height: Double(moleScale)))
        }

        root.addChildNode(headNode)
    }
}

// MARK: - Category Grid View
public struct MiiItemPickerGrid: View {
    let title: String
    let totalCount: Int
    let iconPrefix: String
    @Binding var selectedIndex: Int

    private let columns = [GridItem(.adaptive(minimum: 64, maximum: 80), spacing: 8)]

    public var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.headline)
                .foregroundColor(.white)
            ScrollView {
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(0..<totalCount, id: \.self) { idx in
                        let name = String(format: "%@%02d", iconPrefix, idx)
                        Button(action: { selectedIndex = idx }) {
                            ZStack {
                                RoundedRectangle(cornerRadius: 10)
                                    .fill(selectedIndex == idx ? Color.accentColor : Color(white: 0.2))
                                    .frame(width: 68, height: 60)
                                if let img = MiiIconProvider.shared.icon(named: name) {
                                    Image(uiImage: img)
                                        .resizable()
                                        .aspectRatio(contentMode: .fit)
                                        .frame(width: 54, height: 46)
                                } else {
                                    Text("\(idx)")
                                        .font(.subheadline)
                                        .foregroundColor(.white)
                                }
                            }
                        }
                        .buttonStyle(PlainButtonStyle())
                    }
                }
                .padding(.vertical, 4)
            }
            .frame(maxHeight: 180)
        }
    }
}

// MARK: - Color Picker Row
public struct MiiColorPickerRow: View {
    let title: String
    let colors: [(name: String, color: Color, uiColor: UIColor)]
    @Binding var selectedIndex: Int

    public var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.subheadline)
                .foregroundColor(Color(white: 0.8))
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(0..<colors.count, id: \.self) { idx in
                        Button(action: { selectedIndex = idx }) {
                            ZStack {
                                Circle()
                                    .fill(colors[idx].color)
                                    .frame(width: 32, height: 32)
                                if selectedIndex == idx {
                                    Circle()
                                        .stroke(Color.white, lineWidth: 3)
                                        .frame(width: 36, height: 36)
                                }
                            }
                        }
                        .buttonStyle(PlainButtonStyle())
                    }
                }
                .padding(.horizontal, 4)
                .padding(.vertical, 4)
            }
        }
    }
}

// MARK: - Fine Slider Row
public struct MiiSliderRow: View {
    let title: String
    @Binding var value: Int
    let range: ClosedRange<Int>

    public var body: some View {
        HStack {
            Text(title)
                .font(.subheadline)
                .foregroundColor(.white)
                .frame(width: 110, alignment: .leading)

            Button(action: { if value > range.lowerBound { value -= 1 } }) {
                Image(systemName: "minus.circle.fill")
                    .foregroundColor(Color(white: 0.7))
            }
            .buttonStyle(PlainButtonStyle())

            Slider(value: Binding(
                get: { Double(value) },
                set: { value = Int($0) }
            ), in: Double(range.lowerBound)...Double(range.upperBound), step: 1.0)
            .accentColor(.blue)

            Button(action: { if value < range.upperBound { value += 1 } }) {
                Image(systemName: "plus.circle.fill")
                    .foregroundColor(Color(white: 0.7))
            }
            .buttonStyle(PlainButtonStyle())

            Text("\(value)")
                .font(.caption)
                .foregroundColor(Color(white: 0.7))
                .frame(width: 28, alignment: .trailing)
        }
    }
}

// MARK: - Main Mii Editor View
public struct MiiEditorView: View {
    @ObservedObject var mii: MiiModelData
    let onSave: () -> Void
    let onCancel: () -> Void

    enum Tab: String, CaseIterable {
        case general = "General"
        case face = "Face"
        case hair = "Hair"
        case eyebrows = "Eyebrows"
        case eyes = "Eyes"
        case nose = "Nose"
        case mouth = "Mouth"
        case facialHair = "Beard"
        case extras = "Glasses/Mole"
    }

    @State private var selectedTab: Tab = .general
    @State private var cameraRotation: Float = 0.0

    public var body: some View {
        ZStack {
            Color(white: 0.08).edgesIgnoringSafeArea(.all)

            VStack(spacing: 0) {
                // Top Header Bar
                HStack {
                    Button("Cancel") {
                        onCancel()
                    }
                    .font(.headline)
                    .foregroundColor(.red)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                    .background(Color(white: 0.2))
                    .cornerRadius(8)

                    Spacer()

                    HStack(spacing: 8) {
                        Text("Name:")
                            .font(.headline)
                            .foregroundColor(.white)
                        TextField("Nickname", text: $mii.nickname)
                            .textFieldStyle(RoundedBorderTextFieldStyle())
                            .frame(width: 140)
                    }

                    Spacer()

                    Button("Save Mii") {
                        onSave()
                    }
                    .font(.headline)
                    .foregroundColor(.white)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                    .background(Color.blue)
                    .cornerRadius(8)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 10)
                .background(Color(white: 0.14))

                // Tab Selector Bar
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(Tab.allCases, id: \.self) { tab in
                            Button(action: { selectedTab = tab }) {
                                Text(tab.rawValue)
                                    .font(.subheadline)
                                    .fontWeight(selectedTab == tab ? .bold : .regular)
                                    .padding(.horizontal, 12)
                                    .padding(.vertical, 6)
                                    .background(selectedTab == tab ? Color.blue : Color(white: 0.2))
                                    .foregroundColor(.white)
                                    .cornerRadius(6)
                            }
                            .buttonStyle(PlainButtonStyle())
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                }
                .background(Color(white: 0.12))

                // Content Area: Split 3D view on Left, Controls on Right
                HStack(spacing: 0) {
                    // Left Column: 3D Viewport
                    VStack(spacing: 6) {
                        MiiSceneView(mii: mii, cameraRotation: $cameraRotation)
                            .cornerRadius(12)
                            .overlay(
                                RoundedRectangle(cornerRadius: 12)
                                    .stroke(Color(white: 0.25), lineWidth: 1)
                            )
                            .padding(.leading, 16)
                            .padding(.top, 10)
                            .padding(.bottom, 6)

                        // Controller Hints Footer
                        HStack(spacing: 12) {
                            Text("🎮 [L]/[R] Tabs")
                            Text("•")
                            Text("[A] Confirm")
                            Text("•")
                            Text("[B] Cancel")
                            Text("•")
                            Text("[Start] Save")
                        }
                        .font(.caption2)
                        .foregroundColor(Color(white: 0.6))
                        .padding(.bottom, 8)
                    }
                    .frame(width: 320)

                    // Right Column: Tab Content
                    ScrollView {
                        VStack(alignment: .leading, spacing: 14) {
                            tabContent
                        }
                        .padding(16)
                    }
                    .background(Color(white: 0.10))
                }
            }
        }
        .onAppear {
            setupControllerHandlers()
        }
    }

    @ViewBuilder
    private var tabContent: some View {
        switch selectedTab {
        case .general:
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Gender:")
                        .foregroundColor(.white)
                    Picker("Gender", selection: $mii.gender) {
                        Text("Male").tag(0)
                        Text("Female").tag(1)
                    }
                    .pickerStyle(SegmentedPickerStyle())
                    .frame(width: 180)
                }

                MiiColorPickerRow(title: "Favorite Color",
                                  colors: MiiColors.favoriteColors,
                                  selectedIndex: $mii.favoriteColor)

                Divider().background(Color(white: 0.3))

                MiiSliderRow(title: "Height", value: $mii.height, range: 0...128)
                MiiSliderRow(title: "Build", value: $mii.build, range: 0...128)
            }

        case .face:
            VStack(alignment: .leading, spacing: 14) {
                MiiItemPickerGrid(title: "Face Shape", totalCount: 12, iconPrefix: "IconFaceline", selectedIndex: $mii.facelineType)
                MiiColorPickerRow(title: "Skin Tone", colors: MiiColors.skinTones, selectedIndex: $mii.facelineColor)
                MiiItemPickerGrid(title: "Wrinkles", totalCount: 12, iconPrefix: "IconFaceT_line", selectedIndex: $mii.facelineWrinkle)
                MiiItemPickerGrid(title: "Makeup / Blush", totalCount: 12, iconPrefix: "IconFaceT_make", selectedIndex: $mii.facelineMake)
            }

        case .hair:
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Toggle("Flip Hair Direction", isOn: Binding(
                        get: { mii.hairFlip == 1 },
                        set: { mii.hairFlip = $0 ? 1 : 0 }
                    ))
                    .foregroundColor(.white)
                }
                MiiColorPickerRow(title: "Hair Color", colors: MiiColors.hairColors, selectedIndex: $mii.hairColor)
                MiiItemPickerGrid(title: "Hair Style (132 Options)", totalCount: 132, iconPrefix: "IconHair", selectedIndex: $mii.hairType)
            }

        case .eyebrows:
            VStack(alignment: .leading, spacing: 14) {
                MiiColorPickerRow(title: "Eyebrow Color", colors: MiiColors.hairColors, selectedIndex: $mii.eyebrowColor)
                MiiItemPickerGrid(title: "Eyebrow Type", totalCount: 24, iconPrefix: "IconEyebrow", selectedIndex: $mii.eyebrowType)
                Divider().background(Color(white: 0.3))
                MiiSliderRow(title: "Scale", value: $mii.eyebrowScale, range: 0...8)
                MiiSliderRow(title: "Rotation", value: $mii.eyebrowRotate, range: 0...11)
                MiiSliderRow(title: "Spread (X)", value: $mii.eyebrowX, range: 0...12)
                MiiSliderRow(title: "Position (Y)", value: $mii.eyebrowY, range: 3...18)
            }

        case .eyes:
            VStack(alignment: .leading, spacing: 14) {
                MiiColorPickerRow(title: "Eye Color", colors: MiiColors.eyeColors, selectedIndex: $mii.eyeColor)
                MiiItemPickerGrid(title: "Eye Style (60 Options)", totalCount: 60, iconPrefix: "IconEye", selectedIndex: $mii.eyeType)
                Divider().background(Color(white: 0.3))
                MiiSliderRow(title: "Scale", value: $mii.eyeScale, range: 0...7)
                MiiSliderRow(title: "Rotation", value: $mii.eyeRotate, range: 0...7)
                MiiSliderRow(title: "Spread (X)", value: $mii.eyeX, range: 0...12)
                MiiSliderRow(title: "Position (Y)", value: $mii.eyeY, range: 0...18)
            }

        case .nose:
            VStack(alignment: .leading, spacing: 14) {
                MiiItemPickerGrid(title: "Nose Style", totalCount: 18, iconPrefix: "IconNose", selectedIndex: $mii.noseType)
                Divider().background(Color(white: 0.3))
                MiiSliderRow(title: "Scale", value: $mii.noseScale, range: 0...8)
                MiiSliderRow(title: "Position (Y)", value: $mii.noseY, range: 0...18)
            }

        case .mouth:
            VStack(alignment: .leading, spacing: 14) {
                MiiItemPickerGrid(title: "Mouth Style (36 Options)", totalCount: 36, iconPrefix: "IconMouth", selectedIndex: $mii.mouthType)
                Divider().background(Color(white: 0.3))
                MiiSliderRow(title: "Scale", value: $mii.mouthScale, range: 0...8)
                MiiSliderRow(title: "Position (Y)", value: $mii.mouthY, range: 0...18)
            }

        case .facialHair:
            VStack(alignment: .leading, spacing: 14) {
                MiiColorPickerRow(title: "Facial Hair Color", colors: MiiColors.hairColors, selectedIndex: $mii.beardColor)
                MiiItemPickerGrid(title: "Mustache Style", totalCount: 6, iconPrefix: "IconMustache", selectedIndex: $mii.mustacheType)
                MiiSliderRow(title: "Mustache Scale", value: $mii.mustacheScale, range: 0...8)
                MiiSliderRow(title: "Mustache (Y)", value: $mii.mustacheY, range: 0...16)
                Divider().background(Color(white: 0.3))
                MiiItemPickerGrid(title: "Beard Style", totalCount: 4, iconPrefix: "IconBeard", selectedIndex: $mii.beardType)
            }

        case .extras:
            VStack(alignment: .leading, spacing: 14) {
                MiiItemPickerGrid(title: "Glasses Style", totalCount: 20, iconPrefix: "IconGlass", selectedIndex: $mii.glassType)
                MiiColorPickerRow(title: "Glasses Color", colors: MiiColors.glassesColors, selectedIndex: $mii.glassColor)
                MiiSliderRow(title: "Glasses Scale", value: $mii.glassScale, range: 0...7)
                MiiSliderRow(title: "Glasses (Y)", value: $mii.glassY, range: 0...20)

                Divider().background(Color(white: 0.3))

                Toggle("Enable Beauty Mark (Mole)", isOn: Binding(
                    get: { mii.moleType == 1 },
                    set: { mii.moleType = $0 ? 1 : 0 }
                ))
                .foregroundColor(.white)

                if mii.moleType == 1 {
                    MiiSliderRow(title: "Mole Scale", value: $mii.moleScale, range: 0...8)
                    MiiSliderRow(title: "Mole (X)", value: $mii.moleX, range: 0...16)
                    MiiSliderRow(title: "Mole (Y)", value: $mii.moleY, range: 0...30)
                }
            }
        }
    }

    private func setupControllerHandlers() {
        guard let controller = GCController.current ?? GCController.controllers().first else { return }
        controller.extendedGamepad?.leftShoulder.pressedChangedHandler = { _, _, pressed in
            if pressed {
                let all = Tab.allCases
                if let idx = all.firstIndex(of: selectedTab), idx > 0 {
                    selectedTab = all[idx - 1]
                }
            }
        }
        controller.extendedGamepad?.rightShoulder.pressedChangedHandler = { _, _, pressed in
            if pressed {
                let all = Tab.allCases
                if let idx = all.firstIndex(of: selectedTab), idx < all.count - 1 {
                    selectedTab = all[idx + 1]
                }
            }
        }
        controller.extendedGamepad?.buttonB.pressedChangedHandler = { _, _, pressed in
            if pressed { onCancel() }
        }
        controller.extendedGamepad?.buttonMenu.pressedChangedHandler = { _, _, pressed in
            if pressed { onSave() }
        }
        controller.extendedGamepad?.rightThumbstick.valueChangedHandler = { _, x, _ in
            cameraRotation += x * 0.05
        }
    }
}

// MARK: - C Bridge
@_cdecl("dm_show_mii_editor")
public func dmShowMiiEditor(_ charinfoPtr: UnsafeMutableRawPointer, _ isNew: Int32) -> Int32 {
    let mii = MiiModelData()
    mii.load(from: charinfoPtr)

    let done = DispatchSemaphore(value: 0)
    var saved: Int32 = 0

    DispatchQueue.main.async {
        var topVC: UIViewController? = nil
        for scene in UIApplication.shared.connectedScenes {
            if let ws = scene as? UIWindowScene {
                for window in ws.windows where window.isKeyWindow {
                    topVC = window.rootViewController
                }
            }
        }
        while let presented = topVC?.presentedViewController {
            topVC = presented
        }

        guard let presenter = topVC else {
            done.signal()
            return
        }

        let editorView = MiiEditorView(mii: mii, onSave: {
            saved = 1
            mii.save(to: charinfoPtr)
            presenter.dismiss(animated: true) {
                done.signal()
            }
        }, onCancel: {
            saved = 0
            presenter.dismiss(animated: true) {
                done.signal()
            }
        })

        let hosting = DmHostingController(rootView: editorView)
        hosting.modalPresentationStyle = .fullScreen
        presenter.present(hosting, animated: true, completion: nil)
    }

    done.wait()
    return saved
}
