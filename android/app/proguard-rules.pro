# kotlinx.serialization keeps its generated serializers reachable through
# reflection on the companion; without these the release build fails to
# deserialize API responses.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.**

-keepclassmembers class **$$serializer { *; }
-keepclasseswithmembers class org.gaindrive.android.net.** {
	*** Companion;
}
-keepclassmembers class org.gaindrive.android.net.** {
	kotlinx.serialization.KSerializer serializer(...);
}

# Retrofit keeps generic signatures of service methods.
-keepattributes Signature, RuntimeVisibleAnnotations, AnnotationDefault
-keep,allowobfuscation,allowshrinking interface retrofit2.Call
-keep,allowobfuscation,allowshrinking class retrofit2.Response
-keep,allowobfuscation,allowshrinking class kotlin.coroutines.Continuation
