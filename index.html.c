const { onCall, HttpsError } = require("firebase-functions/v2/https");
const { setGlobalOptions } = require("firebase-functions/v2");
const { defineSecret } = require("firebase-functions/params");
const admin = require("firebase-admin");
const { GoogleGenerativeAI } = require("@google/generative-ai");
admin.initializeApp();
setGlobalOptions({ region: "asia-south1", maxInstances: 10 });
const db = admin.firestore();
const GEMINI_API_KEY = defineSecret("GEMINI_API_KEY");
function requireAuth(req) {
  if (!req.auth) throw new HttpsError("unauthenticated", "Please sign in.");
  return req.auth;
}
function requireTeacher(req) {
  const a = requireAuth(req);
  if (a.token.teacher !== true)
    throw new HttpsError("permission-denied", "Teacher access required.");
  return a;
}
async function getStudentByUid(uid) {
  const q = await db
    .collection("students")
    .where("authUid", "==", uid)
    .limit(1)
    .get();
  if (q.empty)
    throw new HttpsError(
      "not-found",
      "No student profile is linked to this account."
    );
  const doc = q.docs[0];
  return { ref: doc.ref, data: doc.data() };
}
exports.getMyAttendance = onCall(async (req) => {
  const auth = requireAuth(req);
  const { ref, data } = await getStudentByUid(auth.uid);
  const snap = await ref.collection("attendance").get();
  return {
    id: data.studentId,
    name: data.name,
    department: data.department,
    year: data.year,
    semester: data.semester,
    subjects: snap.docs.map((d) => ({ subjectId: d.id, ...d.data() })),
  };
});
exports.markAttendance = onCall(async (req) => {
  requireTeacher(req);
  const { studentId, subject, sessionDate, status } = req.data || {};
  if (
    !studentId ||
    !subject ||
    !/^\\d{4}-\\d{2}-\\d{2}$/.test(sessionDate || "") ||
    !["Present", "Absent"].includes(status)
  )
    throw new HttpsError(
      "invalid-argument",
      "Provide a student, subject, valid date and attendance status."
    );
  const s = await db.collection("students").doc(studentId).get();
  if (!s.exists) throw new HttpsError("not-found", "Student not found.");
  const subjects = await s.ref
    .collection("attendance")
    .where("subjectName", "==", subject)
    .limit(1)
    .get();
  if (subjects.empty)
    throw new HttpsError("not-found", "Subject not found for this student.");
  const subjectRef = subjects.docs[0].ref;
  const sessionId = `${studentId}_${subjectRef.id}_${sessionDate}`;
  const sessionRef = s.ref.collection("sessions").doc(sessionId);
  await db.runTransaction(async (tx) => {
    const existing = await tx.get(sessionRef);
    if (existing.exists)
      throw new HttpsError(
        "already-exists",
        "Attendance already recorded for this student, subject and class date."
      );
    const a = await tx.get(subjectRef);
    const v = a.data();
    tx.create(sessionRef, {
      studentId,
      subjectId: subjectRef.id,
      subjectName: subject,
      sessionDate,
      status,
      createdAt: admin.firestore.FieldValue.serverTimestamp(),
      createdBy: req.auth.uid,
    });
    tx.update(subjectRef, {
      total: Number(v.total || 0) + 1,
      attended: Number(v.attended || 0) + (status === "Present" ? 1 : 0),
      missed: Number(v.missed || 0) + (status === "Absent" ? 1 : 0),
      updatedAt: admin.firestore.FieldValue.serverTimestamp(),
    });
  });
  return { success: true, message: "Attendance saved." };
});
exports.attendanceAssistant = onCall(
  { secrets: [GEMINI_API_KEY] },
  async (req) => {
    const auth = requireAuth(req);
    const question = String(req.data?.question || "").slice(0, 500);
    if (!question.trim())
      throw new HttpsError("invalid-argument", "Enter a question.");
    const { data } = await getStudentByUid(auth.uid);
    const snap = await db
      .collection("students")
      .doc(data.studentId)
      .collection("attendance")
      .get();
    const subjects = snap.docs.map((d) => ({
      name: d.data().subjectName,
      total: d.data().total,
      attended: d.data().attended,
      missed: d.data().missed,
      percentage: d.data().total
        ? Number(((100 * d.data().attended) / d.data().total).toFixed(2))
        : 0,
      classesTo75: Math.max(
        0,
        Math.ceil((0.75 * d.data().total - d.data().attended) / 0.25)
      ),
    }));
    const genAI = new GoogleGenerativeAI(GEMINI_API_KEY.value());
    const model = genAI.getGenerativeModel({ model: "gemini-1.5-flash" });
    const prompt = `You are a college attendance assistant. Answer only from this JSON attendance data. Do not invent data. Explain that classesTo75 assumes no further absences. Minimum required attendance is 75%. If the data cannot answer, say so. Keep answer student-friendly.\\nDATA: ${JSON.stringify(
      subjects
    )}\\nQUESTION: ${question}`;
    const result = await model.generateContent(prompt);
    return { answer: result.response.text(), subjects };
  }
);
    